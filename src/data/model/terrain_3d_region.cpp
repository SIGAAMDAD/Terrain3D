// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "data/model/terrain_3d_region.h"
#include "core/utility/terrain_3d_binary_io.h"

#include "core/utility/logger.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_util.h"
#include "data/model/terrain_3d_data.h"
#include "instances/storage/terrain_3d_instance_page_codec.h"

#include <string.h>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/time.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static bool t3dRegionImageIsDefaultControl(
	const Ref< Image > &rMap,
	const int			nRegionSize ) {
	if ( rMap.is_null() || rMap->get_format() != Image::FORMAT_RF || rMap->get_width() != nRegionSize ||
		 rMap->get_height() != nRegionSize ) {
		return false;
	}
	const PackedByteArray data = rMap->get_data();
	const size_t		  texel_count = ( size_t ) ( nRegionSize ) * ( size_t ) ( nRegionSize );
	if ( ( size_t ) ( data.size() ) != texel_count * sizeof( uint32_t ) ) {
		return false;
	}
	const uint8_t *pBytes = data.ptr();
	for ( size_t index = 0; index < texel_count; ++index ) {
		const uint32_t value = t3dReadNativeUnaligned< uint32_t >( pBytes + index * sizeof( uint32_t ) );
		if ( value != T3D_REGION_DEFAULT_CONTROL ) {
			return false;
		}
	}
	return true;
}

static bool t3dRegionImageIsDefaultColor(
	const Ref< Image > &rMap,
	const int			nRegionSize ) {
	if ( rMap.is_null() || rMap->get_format() != Image::FORMAT_RGBA8 || rMap->get_width() != nRegionSize ||
		 rMap->get_height() != nRegionSize ) {
		return false;
	}
	const PackedByteArray data = rMap->get_data();
	const size_t		  texel_count = ( size_t ) ( nRegionSize ) * ( size_t ) ( nRegionSize );
	if ( ( size_t ) ( data.size() ) != texel_count * 4u ) {
		return false;
	}
	const uint32_t rgba = COLOR_ROUGHNESS.to_rgba32();
	const uint8_t  expected[4] = { ( uint8_t ) ( rgba >> 24u ),
								   ( uint8_t ) ( rgba >> 16u ),
								   ( uint8_t ) ( rgba >> 8u ),
								   ( uint8_t ) ( rgba ) };
	const uint8_t *pBytes = data.ptr();
	for ( size_t index = 0; index < texel_count; ++index ) {
		const uint8_t *pixel = pBytes + index * 4u;
		if ( pixel[0] != expected[0] || pixel[1] != expected[1] || pixel[2] != expected[2] ||
			 pixel[3] != expected[3] ) {
			return false;
		}
	}
	return true;
}

EA_FORCE_INLINE static int t3dRegionNormalizeStreamedNodeBoundaryPolicy(
	const int nPolicy ) {
	return nPolicy == STREAMED_NODE_BOUNDARY_COVERAGE ? STREAMED_NODE_BOUNDARY_COVERAGE : STREAMED_NODE_BOUNDARY_ANCHOR;
}

static void t3dRegionNormalizeStreamedNodeCoverage(
	const int		nPolicy,
	const Vector2i &rInputMin,
	const Vector2i &rInputMax,
	Vector2i	   &rOutputMin,
	Vector2i	   &rOutputMax ) {
	if ( nPolicy != STREAMED_NODE_BOUNDARY_COVERAGE ) {
		rOutputMin = s_v2iZero;
		rOutputMax = s_v2iZero;
		return;
	}
	const Vector2i inputMin = rInputMin;
	const Vector2i inputMax = rInputMax;
	rOutputMin = Vector2i( MIN( inputMin.x, inputMax.x ), MIN( inputMin.y, inputMax.y ) );
	rOutputMax = Vector2i( MAX( inputMin.x, inputMax.x ), MAX( inputMin.y, inputMax.y ) );
}

EA_FORCE_INLINE static int t3dRegionNormalizeLightingClass(
	const int nClass ) {
	return CLAMP( nClass, ( int ) ( LIGHTING_CLASS_TERRAIN ), ( int ) ( LIGHTING_CLASS_REGIONAL_OCCLUDER ) );
}

EA_FORCE_INLINE static int t3dRegionNormalizeBakedLightingMode(
	const int nMode ) {
	return CLAMP( nMode, ( int ) ( BAKED_LIGHTING_DISABLED ), ( int ) ( BAKED_LIGHTING_MATERIAL_ONLY ) );
}

static void t3dRegionStoreStreamedNodeLighting(
	Dictionary		 &rEntry,
	const Dictionary &dictSource ) {
	rEntry["lighting_class"] = t3dRegionNormalizeLightingClass( dictSource.has( "lighting_class" )
																	? ( int ) ( dictSource["lighting_class"] )
																	: ( int ) ( LIGHTING_CLASS_MEDIUM_OBSTACLE ) );
	rEntry["baked_lighting_mode"] = t3dRegionNormalizeBakedLightingMode(
		dictSource.has( "baked_lighting_mode" ) ? ( int ) ( dictSource["baked_lighting_mode"] )
												: ( int ) ( BAKED_LIGHTING_LIGHTMAP ) );
	rEntry["contribute_baked_gi"] =
		dictSource.has( "contribute_baked_gi" ) ? ( bool ) ( dictSource["contribute_baked_gi"] ) : true;
	rEntry["receive_dynamic_directional"] =
		dictSource.has( "receive_dynamic_directional" ) ? ( bool ) ( dictSource["receive_dynamic_directional"] ) : true;
	rEntry["dynamic_shadow_eligible"] =
		dictSource.has( "dynamic_shadow_eligible" ) ? ( bool ) ( dictSource["dynamic_shadow_eligible"] ) : true;
	if ( dictSource.has( "shadow_proxy_scene_path" ) && !String( dictSource["shadow_proxy_scene_path"] ).is_empty() ) {
		rEntry["shadow_proxy_scene_path"] = String( dictSource["shadow_proxy_scene_path"] );
	} else if ( dictSource.has( "shadow_proxy_scene" ) &&
				Variant( dictSource["shadow_proxy_scene"] ).get_type() == Variant::OBJECT ) {
		rEntry["shadow_proxy_scene"] = dictSource["shadow_proxy_scene"];
	}
}

static void t3dRegionStoreStreamedNodeBoundary(
	Dictionary	   &rEntry,
	const int		nPolicy,
	const Vector2i &rCoverageMin,
	const Vector2i &rCoverageMax ) {
	const int policy = t3dRegionNormalizeStreamedNodeBoundaryPolicy( nPolicy );
	Vector2i  coverage_min;
	Vector2i  coverage_max;
	t3dRegionNormalizeStreamedNodeCoverage( policy, rCoverageMin, rCoverageMax, coverage_min, coverage_max );
	rEntry["boundary_policy"] = policy;
	rEntry["coverage_min"] = coverage_min;
	rEntry["coverage_max"] = coverage_max;
}

// -----------------------------------------------------------------------------
// Public functions

Terrain3DRegion::Terrain3DRegion(
	void ) :
	m_streamedNodeRuntime( Terrain3DEastlAllocator::bootstrapAllocator() ) {
}


Terrain3DRegion::~Terrain3DRegion(
	void ) {
}

void Terrain3DRegion::clear(
	void ) {
	m_fVersion = 0.8f;
	m_heightRange = s_v2Zero;
	m_heightMap.unref();
	m_controlMap.unref();
	m_colorMap.unref();
	m_derivedNormalSlopeMap.unref();
	m_bDerivedNormalSlopeValid = false;
	m_nChannelFlags = 0u;
	m_bRuntimeMapsModified = false;
	m_nRasterDirtyDomains = 0u;
	m_bInstancesDirty = false;
	m_bMetadataDirty = false;
	m_instanceStore.clear();
	m_instancePage.clear();
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = false;
	m_nCookedPageSourceSignature = 0u;
	m_nMegaVTSourceGeneration = 1u;
	m_nInstanceGeneration = 0;
	m_nMegaVTDirtyMip0Mask = 0u;
	clearInstanceMetadata();
	m_streamedNodes.clear();
	m_streamedNodeRuntime.clear();
	m_nNextStreamedNodeId = 1u;
	m_streamedNodeCoverageMin = s_v2iZero;
	m_streamedNodeCoverageMax = s_v2iZero;
	m_bStreamedNodeHasExtendedCoverage = false;
	m_lighting.unref();
	m_bGeometryContractValid = true;
	m_bDeleted = false;
	m_bEdited = false;
	m_bModified = false;
	m_location = s_v2iMax;
}

void Terrain3DRegion::setVersion(
	const real_t fVersion ) {
	const real_t version = CLAMP( fVersion, 0.8f, 100.f );
	if ( m_fVersion == version ) {
		return;
	}
	// Mark modified if already initialized and we get a new value
	if ( m_fVersion > 0.8f ) {
		m_bMetadataDirty = true; markAuthoringSourceChanged();
		m_bModified = true;
	}
	m_fVersion = version;
	LOG( DEBUG, vformat( "%.3f", m_fVersion ) );
	if ( m_fVersion < s_fDataCurrentVersion ) {
		LOG( WARN,
			 "Region Data ",
			 get_path(),
			 " version ",
			 vformat( "%.3f", m_fVersion ),
			 " will be updated to ",
			 vformat( "%.3f", s_fDataCurrentVersion ),
			 " upon save" );
	}
}

void Terrain3DRegion::setMap(
	const t3dMapType_t	mapType,
	const Ref< Image > &rImage ) {
	switch ( mapType ) {
	case TYPE_HEIGHT:
		setHeightMap( rImage );
		break;
	case TYPE_CONTROL:
		setControlMap( rImage );
		break;
	case TYPE_COLOR:
		setColorMap( rImage );
		break;
	default:
		LOG( ERROR, "Requested map type is invalid" );
		break;
	}
}

Ref< Image > Terrain3DRegion::getMap(
	const t3dMapType_t mapType ) const {
	switch ( mapType ) {
	case TYPE_HEIGHT:
		return getHeightMap();
	case TYPE_CONTROL:
		return getControlMap();
	case TYPE_COLOR:
		return getColorMap();
	default:
		LOG( ERROR, "Requested map type ", mapType, ", is invalid" );
		return Ref< Image >();
	}
}

Image *Terrain3DRegion::getMapPtr(
	const t3dMapType_t mapType ) const {
	switch ( mapType ) {
	case TYPE_HEIGHT:
		return m_heightMap.is_valid() ? *m_heightMap : NULL;
	case TYPE_CONTROL:
		return m_controlMap.is_valid() ? *m_controlMap : NULL;
	case TYPE_COLOR:
		return m_colorMap.is_valid() ? *m_colorMap : NULL;
	default:
		LOG( ERROR, "Requested map type ", mapType, ", is invalid" );
		return NULL;
	}
}

Ref< Image > Terrain3DRegion::materializeMap(
	const t3dMapType_t mapType ) {
	Ref< Image > pMap = getMap( mapType );
	if ( pMap.is_valid() ) {
		return pMap;
	}
	if ( !t3dIsValidRegionSize( T3D_REGION_SIZE ) ) {
		LOG( ERROR, "Cannot materialize map before region_size is valid" );
		return Ref< Image >();
	}
	if ( mapType == TYPE_HEIGHT ) {
		pMap = Terrain3DUtil::getFilledImage( V2I( T3D_REGION_SIZE ),
											  t3dRegionCodecDefaultHeightColor(),
											  false,
											  Image::FORMAT_R16 );
	} else if ( mapType == TYPE_CONTROL ) {
		pMap = Terrain3DUtil::getFilledImage( V2I( T3D_REGION_SIZE ), COLOR_CONTROL, false, Image::FORMAT_RF );
	} else if ( mapType == TYPE_COLOR ) {
		// CPU region color maps retain only their base level. GPU publication creates
		// a temporary mipmapped copy, avoiding 33% permanent CPU mip overhead.
		pMap = Terrain3DUtil::getFilledImage( V2I( T3D_REGION_SIZE ), COLOR_ROUGHNESS, false, Image::FORMAT_RGBA8 );
	}
	if ( pMap.is_null() ) {
		return Ref< Image >();
	}

	// Writable materialization deliberately bypasses setControlMap() and setColorMap().
	// Those setters compact all-default images back to null, which is correct for
	// persistent assignment but would make an implicit channel impossible to edit.
	// Merely allocating the writable image is not itself an authored edit; callers
	// commit or compact it after they know whether any texel actually changed.
	if ( mapType == TYPE_HEIGHT ) {
		setHeightMap( pMap );
		return getHeightMap();
	} else if ( mapType == TYPE_CONTROL ) {
		m_controlMap = pMap;
		m_nChannelFlags |= REGION_CHANNEL_CONTROL;
	} else if ( mapType == TYPE_COLOR ) {
		m_colorMap = pMap;
		m_nChannelFlags |= REGION_CHANNEL_COLOR;
	}
	return pMap;
}

Image *Terrain3DRegion::getMapPtrForWrite(
	const t3dMapType_t mapType ) {
	Ref< Image > pMap = materializeMap( mapType );
	return pMap.is_valid() ? *pMap : NULL;
}

void Terrain3DRegion::setMaps(
	const TypedArray< Image > &rMaps ) {
	if ( rMaps.size() != REGION_TYPE_MAX ) {
		LOG( ERROR, "Expected ", REGION_TYPE_MAX - 1, " maps. Received ", rMaps.size() );
		return;
	}
	setHeightMap( rMaps[TYPE_HEIGHT] );
	setControlMap( rMaps[TYPE_CONTROL] );
	setColorMap( rMaps[TYPE_COLOR] );
}

TypedArray< Image > Terrain3DRegion::getMaps(
	void ) const {
	LOG( DEBUG, "Retrieving maps from region: ", m_location );
	TypedArray< Image > maps;
	maps.push_back( m_heightMap );
	maps.push_back( m_controlMap );
	maps.push_back( m_colorMap );
	return maps;
}

void Terrain3DRegion::setHeightMap(
	const Ref< Image > &rMap ) {
	LOG( DEBUG, "Setting height map for region: ", ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)" );
	const Ref< Image > pOldMap = m_heightMap;
	const Ref< Image > pMap = sanitizeMap( TYPE_HEIGHT, rMap );
	if ( rMap.is_valid() && pMap.is_null() ) {
		LOG( ERROR, "Height map assignment was rejected; existing region storage remains unchanged" );
		return;
	}
	if ( pOldMap == pMap ) {
		return;
	}
	m_heightMap = pMap;
	m_bDerivedNormalSlopeValid = false;
	markAuthoringSourceChanged();
	m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
	m_bRuntimeMapsModified = true;
	m_bModified = true;
	calcHeightRange();
}

void Terrain3DRegion::setControlMap(
	const Ref< Image > &rMap ) {
	LOG( DEBUG, "Setting control map for region: ", ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)" );
	const Ref< Image > pOldMap = m_controlMap;
	Ref< Image >	   pMap = sanitizeMap( TYPE_CONTROL, rMap );
	if ( t3dRegionImageIsDefaultControl( pMap, T3D_REGION_SIZE ) ) {
		pMap.unref();
	}
	if ( pOldMap == pMap ) {
		return;
	}
	m_controlMap = pMap;
	m_nRasterDirtyDomains |= DIRTY_CONTROL | DIRTY_RASTER_INDEX;
	markAuthoringSourceChanged();
	if ( m_controlMap.is_valid() ) {
		m_nChannelFlags |= REGION_CHANNEL_CONTROL;
	} else {
		m_nChannelFlags &= ( uint8_t ) ( ~REGION_CHANNEL_CONTROL );
	}
	m_bRuntimeMapsModified = true;
	m_bModified = true;
}

void Terrain3DRegion::setColorMap(
	const Ref< Image > &rMap ) {
	LOG( DEBUG, "Setting color map for region: ", ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)" );
	const Ref< Image > pOldMap = m_colorMap;
	Ref< Image >	   pMap = sanitizeMap( TYPE_COLOR, rMap );
	if ( t3dRegionImageIsDefaultColor( pMap, T3D_REGION_SIZE ) ) {
		pMap.unref();
	}
	if ( pOldMap == pMap ) {
		return;
	}
	m_colorMap = pMap;
	m_nRasterDirtyDomains |= DIRTY_COLOR | DIRTY_RASTER_INDEX;
	markAuthoringSourceChanged();
	if ( m_colorMap.is_valid() ) {
		m_nChannelFlags |= REGION_CHANNEL_COLOR;
	} else {
		m_nChannelFlags &= ( uint8_t ) ( ~REGION_CHANNEL_COLOR );
	}
	m_bRuntimeMapsModified = true;
	m_bModified = true;
}

void Terrain3DRegion::setChannelFlags(
	const int nFlags ) {
	uint8_t flags = ( uint8_t ) ( nFlags ) &REGION_CHANNEL_MASK;
	// Image payloads are authoritative when present. This makes property load
	// order irrelevant for old and new .res files.
	if ( m_controlMap.is_valid() ) {
		flags |= REGION_CHANNEL_CONTROL;
	}
	if ( m_colorMap.is_valid() ) {
		flags |= REGION_CHANNEL_COLOR;
	}
	if ( m_derivedNormalSlopeMap.is_valid() ) {
		flags |= REGION_CHANNEL_DERIVED;
	}
	m_nChannelFlags = flags;
}

void Terrain3DRegion::setDerivedNormalSlopeMapInternal(
	const Ref< Image > &rMap ) {
	if ( rMap.is_null() || rMap->get_width() != T3D_REGION_SIZE || rMap->get_height() != T3D_REGION_SIZE ||
		 rMap->get_format() != Image::FORMAT_RG8 ) {
		m_derivedNormalSlopeMap.unref();
		m_bDerivedNormalSlopeValid = false;
		return;
	}
	m_derivedNormalSlopeMap = rMap;
	m_bDerivedNormalSlopeValid = true;
	m_nChannelFlags |= REGION_CHANNEL_DERIVED;
}

void Terrain3DRegion::sanitizeMaps(
	void ) {
	Ref< Image > pMap = sanitizeMap( TYPE_HEIGHT, m_heightMap );
	if ( m_heightMap != pMap ) {
		markAuthoringSourceChanged();
		m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
		m_bRuntimeMapsModified = true;
		m_bModified = true;
	}
	m_heightMap = pMap;
	m_bDerivedNormalSlopeValid = false;
	if ( m_controlMap.is_valid() ) {
		pMap = sanitizeMap( TYPE_CONTROL, m_controlMap );
		if ( m_controlMap != pMap ) {
			markAuthoringSourceChanged();
			m_nRasterDirtyDomains |= DIRTY_CONTROL | DIRTY_RASTER_INDEX;
			m_bRuntimeMapsModified = true;
			m_bModified = true;
		}
		m_controlMap = pMap;
	}
	if ( m_colorMap.is_valid() ) {
		pMap = sanitizeMap( TYPE_COLOR, m_colorMap );
		if ( m_colorMap != pMap ) {
			markAuthoringSourceChanged();
			m_nRasterDirtyDomains |= DIRTY_COLOR | DIRTY_RASTER_INDEX;
			m_bRuntimeMapsModified = true;
			m_bModified = true;
		}
		m_colorMap = pMap;
	}
	setChannelFlags( m_nChannelFlags );
	compactDefaultMaps();
}

bool Terrain3DRegion::isMapDefault(
	const t3dMapType_t mapType ) const {
	switch ( mapType ) {
	case TYPE_CONTROL:
		return m_controlMap.is_null() || t3dRegionImageIsDefaultControl( m_controlMap, T3D_REGION_SIZE );
	case TYPE_COLOR:
		return m_colorMap.is_null() || t3dRegionImageIsDefaultColor( m_colorMap, T3D_REGION_SIZE );
	default:
		return false;
	}
}

bool Terrain3DRegion::compactMapIfDefault(
	const t3dMapType_t mapType,
	const bool		   bMarkModified ) {
	Ref< Image > *pMap = NULL;
	switch ( mapType ) {
	case TYPE_CONTROL:
		pMap = &m_controlMap;
		break;
	case TYPE_COLOR:
		pMap = &m_colorMap;
		break;
	default:
		return false;
	}
	if ( pMap->is_null() || !isMapDefault( mapType ) ) {
		return false;
	}
	pMap->unref();
	if ( mapType == TYPE_CONTROL ) {
		m_nChannelFlags &= ( uint8_t ) ( ~REGION_CHANNEL_CONTROL );
	} else if ( mapType == TYPE_COLOR ) {
		m_nChannelFlags &= ( uint8_t ) ( ~REGION_CHANNEL_COLOR );
	}
	if ( bMarkModified ) {
		m_nRasterDirtyDomains |= (1u << (uint32_t)mapType) | DIRTY_RASTER_INDEX;
		m_bRuntimeMapsModified = true;
		m_bModified = true;
	}
	return true;
}

void Terrain3DRegion::compactDefaultMaps(
	void ) {
	compactMapIfDefault( TYPE_CONTROL );
	compactMapIfDefault( TYPE_COLOR );
}

void Terrain3DRegion::markMapEdited(
	const t3dMapType_t mapType,
	const bool		   bRecalculateHeightRange ) {
	if ( mapType < TYPE_HEIGHT || mapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Invalid map type: ", mapType );
		return;
	}
	m_nRasterDirtyDomains |= (1u << (uint32_t)mapType) | DIRTY_RASTER_INDEX;
	m_bRuntimeMapsModified = true;
	markAuthoringSourceChanged();
	m_bModified = true;
	m_bEdited = true;
	if ( mapType == TYPE_HEIGHT ) {
		m_bDerivedNormalSlopeValid = false;
	}
	if ( mapType == TYPE_HEIGHT && bRecalculateHeightRange ) {
		calcHeightRange();
	}
}

Ref< Image > Terrain3DRegion::sanitizeMap(
	const t3dMapType_t	mapType,
	const Ref< Image > &rMap ) const {
	LOG( DEBUG, "Sanitizing map type: ", mapType, ", map: ", rMap );
	if ( !t3dIsValidRegionSize( T3D_REGION_SIZE ) ) {
		LOG( ERROR,
			 "Invalid region size: ",
			 T3D_REGION_SIZE,
			 ". Set it or set a map first. This fork requires exactly 128 samples per region" );
		return Ref< Image >();
	}
	if ( mapType != TYPE_HEIGHT && rMap.is_null() ) {
		// Missing control/color maps are implicit defaults until first write.
		return Ref< Image >();
	}
	if ( mapType == TYPE_HEIGHT ) {
		if ( rMap.is_valid() && rMap->get_format() == Image::FORMAT_R16 &&
			 !t3dRegionCodecHasValidHeightPayload( rMap, T3D_REGION_SIZE ) ) {
			LOG( ERROR,
				 "Rejected malformed R16 height payload for region ",
				 m_location,
				 ": expected ",
				 t3dRegionCodecExpectedHeightByteCount( T3D_REGION_SIZE, Image::FORMAT_R16 ),
				 " bytes, received ",
				 rMap->get_data().size() );
			return Ref< Image >();
		}

		uint32_t	 clamped_count = 0u;
		uint32_t	 non_finite_count = 0u;
		Ref< Image > pConverted =
			t3dRegionCodecConvertHeightToR16( rMap, T3D_REGION_SIZE, clamped_count, non_finite_count );
		if ( rMap.is_valid() && rMap->get_format() == Image::FORMAT_RF && pConverted.is_valid() ) {
			LOG( INFO,
				 "Converted internal RF32 height storage to R16 for region ",
				 m_location,
				 " (",
				 T3D_REGION_SIZE * T3D_REGION_SIZE,
				 " samples)" );
		}
		if ( clamped_count > 0u || non_finite_count > 0u ) {
			LOG( WARN,
				 "R16 height conversion for region ",
				 m_location,
				 " clamped ",
				 clamped_count,
				 " samples and replaced ",
				 non_finite_count,
				 " non-finite samples" );
		}
		return pConverted;
	}
	const char		   *type_str = s_pszRegionMapTypeNames[mapType];
	const Image::Format format = s_szRegionMapFormats[mapType];
	Color				color = s_szRegionMapDefaultColors( mapType );
	Ref< Image >		pMap;

	if ( rMap.is_valid() ) {
		if ( validateMapSize( rMap ) ) {
			if ( rMap->get_format() == format ) {
				LOG( DEBUG, "Map type ", type_str, " correct format, size. Mipmaps: ", rMap->has_mipmaps() );
				pMap = rMap;
			} else {
				LOG( DEBUG,
					 "Provided ",
					 type_str,
					 " map wrong format: ",
					 rMap->get_format(),
					 ". Converting copy to: ",
					 format );
				pMap.instantiate();
				pMap->copy_from( rMap );
				pMap->convert( format );
				if ( pMap->get_format() != format ) {
					LOG( DEBUG, "Cannot convert image to format: ", format, ". Creating blank " );
					pMap.unref();
				}
			}
		} else {
			LOG( DEBUG, "Provided ", type_str, " map wrong size: ", rMap->get_size(), ". Creating blank" );
		}
	} else {
		LOG( DEBUG, "No provided ", type_str, " map. Creating blank" );
	}
	if ( pMap.is_null() ) {
		LOG( DEBUG, "Making new image of type: ", type_str );
		return Terrain3DUtil::getFilledImage( V2I( T3D_REGION_SIZE ), color, false, format );
	}
	if ( mapType == TYPE_COLOR && pMap->has_mipmaps() ) {
		Ref< Image > pBaseOnly;
		pBaseOnly.instantiate();
		pBaseOnly->copy_from( pMap );
		pBaseOnly->clear_mipmaps();
		return pBaseOnly;
	}
	return pMap;
}

bool Terrain3DRegion::validateMapSize(
	const Ref< Image > &rMap ) const {
	const Vector2i region_sizev = rMap->get_size();
	if ( region_sizev.x != region_sizev.y ) {
		LOG( ERROR, "Image width doesn't match height: ", region_sizev );
		return false;
	}
	if ( !t3dIsValidRegionSize( region_sizev.x ) || !t3dIsValidRegionSize( region_sizev.y ) ) {
		LOG( ERROR, "Invalid image size: ", region_sizev, ". This fork requires a square 128x128 image" );
		return false;
	}
	return true;
}

void Terrain3DRegion::setMegaVTSourceGeneration(
	const int64_t nGeneration ) {
	const uint32_t nSanitized =
		nGeneration <= 0 ? 1u
						 : ( nGeneration > ( int64_t ) ( UINT32_MAX ) ? UINT32_MAX : ( uint32_t ) ( nGeneration ) );
	if ( m_nMegaVTSourceGeneration == nSanitized ) {
		return;
	}
	m_nMegaVTSourceGeneration = nSanitized;
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
}

void Terrain3DRegion::setMegaVTDirtyMip0Mask(
	const int64_t nMask ) {
	const uint32_t nSanitized = nMask <= 0 ? 0u : ( ( uint32_t ) ( nMask ) & 0xffffu );
	if ( m_nMegaVTDirtyMip0Mask == nSanitized ) {
		return;
	}
	m_nMegaVTDirtyMip0Mask = nSanitized;
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
}

void Terrain3DRegion::setMegaVTEditorStateInternal(
	const uint32_t nGeneration,
	const uint32_t nDirtyMip0Mask ) {
	const uint32_t nSanitizedGeneration = nGeneration == 0u ? 1u : nGeneration;
	const uint32_t nSanitizedMask = nDirtyMip0Mask & 0xffffu;
	if ( m_nMegaVTSourceGeneration == nSanitizedGeneration && m_nMegaVTDirtyMip0Mask == nSanitizedMask ) {
		return;
	}
	m_nMegaVTSourceGeneration = nSanitizedGeneration;
	m_nMegaVTDirtyMip0Mask = nSanitizedMask;
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
}

void Terrain3DRegion::setHeightRange(
	const Vector2 &rRange ) {
	if ( t3dDiffers( m_heightRange, rRange ) ) {
		// Mark modified if setting after initialization
		if ( !m_heightRange.is_zero_approx() ) {
			m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
			m_bModified = true;
		}
		m_heightRange = rRange;
		LOG( DEBUG, vformat( "%.2v", rRange ) );
	} else {
		return;
	};
}

void Terrain3DRegion::updateHeight(
	const real_t fHeight ) {
	if ( fHeight < m_heightRange.x ) {
		m_heightRange.x = fHeight;
		m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
		m_bModified = true;
	} else if ( fHeight > m_heightRange.y ) {
		m_heightRange.y = fHeight;
		m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
		m_bModified = true;
	}
}

void Terrain3DRegion::updateHeights(
	const Vector2 &rLowHigh ) {
	if ( rLowHigh.x < m_heightRange.x ) {
		m_heightRange.x = rLowHigh.x;
		m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
		m_bModified = true;
	}
	if ( rLowHigh.y > m_heightRange.y ) {
		m_heightRange.y = rLowHigh.y;
		m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
		m_bModified = true;
	}
}

void Terrain3DRegion::calcHeightRange(
	void ) {
	const Vector2 range = t3dRegionCodecGetHeightMinMax( m_heightMap );
	if ( m_heightRange != range ) {
		m_heightRange = range;
		m_nRasterDirtyDomains |= DIRTY_HEIGHT | DIRTY_RASTER_INDEX;
		m_bModified = true;
		LOG( DEBUG,
			 "Recalculated new height range: ",
			 m_heightRange,
			 " for region: ",
			 ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)",
			 ". Marking modified" );
	}
}

void Terrain3DRegion::adoptCookedRuntimeMapsInternal(
	const Ref< Image > &rHeight,
	const Ref< Image > &rControl,
	const Ref< Image > &rColor,
	const Ref< Image > &rDerived ) {
	if ( rHeight.is_null() || rHeight->get_width() != T3D_REGION_SIZE || rHeight->get_height() != T3D_REGION_SIZE ) {
		return;
	}
	m_heightMap = rHeight;
	m_controlMap = rControl;
	m_colorMap = rColor;
	m_derivedNormalSlopeMap = rDerived;
	m_bDerivedNormalSlopeValid = rDerived.is_valid();
	if ( rControl.is_valid() ) {
		m_nChannelFlags |= REGION_CHANNEL_CONTROL;
	} else {
		m_nChannelFlags &= ~REGION_CHANNEL_CONTROL;
	}
	if ( rColor.is_valid() ) {
		m_nChannelFlags |= REGION_CHANNEL_COLOR;
	} else {
		m_nChannelFlags &= ~REGION_CHANNEL_COLOR;
	}
	if ( rDerived.is_valid() ) {
		m_nChannelFlags |= REGION_CHANNEL_DERIVED;
	} else {
		m_nChannelFlags &= ~REGION_CHANNEL_DERIVED;
	}
	m_bRuntimeMapsModified = false;
}

void Terrain3DRegion::importRuntimeMapsUnmodified(
	const Terrain3DRegion *pSource ) {
	if ( !pSource ) {
		return;
	}
	m_heightMap = pSource->m_heightMap;
	m_controlMap = pSource->m_controlMap;
	m_colorMap = pSource->m_colorMap;
	m_derivedNormalSlopeMap = pSource->m_derivedNormalSlopeMap;
	m_bDerivedNormalSlopeValid = pSource->m_bDerivedNormalSlopeValid;
	m_nChannelFlags = pSource->m_nChannelFlags;
	m_bRuntimeMapsModified = pSource->m_bRuntimeMapsModified;
	m_nRasterDirtyDomains = pSource->m_nRasterDirtyDomains;
	if ( m_bRuntimeMapsModified ) {
		m_bModified = true;
	}
}

void Terrain3DRegion::releaseRuntimeMapsUnmodified(
	void ) {
	m_heightMap.unref();
	m_controlMap.unref();
	m_colorMap.unref();
	m_derivedNormalSlopeMap.unref();
	m_bDerivedNormalSlopeValid = false;
}

void Terrain3DRegion::markRuntimeMapsClean(
	void ) {
	m_bRuntimeMapsModified = false;
	m_nRasterDirtyDomains = 0u;
}

bool Terrain3DRegion::hasRuntimeMapPayload(
	void ) const {
	return m_heightMap.is_valid();
}

bool Terrain3DRegion::runtimeMapsEvictable(
	void ) const {
	return !m_bRuntimeMapsModified && !m_bEdited && !m_bDeleted && !get_path().is_empty();
}

void Terrain3DRegion::clearInstanceMetadata(
	void ) const {
	memset( m_szInstanceMeshCounts, 0, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = 0u;
	m_nInstanceMeshCount = 0u;
}

bool Terrain3DRegion::encodeInstancePage(
	void ) const {
	if ( !m_bInstanceStoreResident ) {
		return true;
	}
	if ( !m_bInstanceStoreModified && !m_instancePage.is_empty() ) {
		return true;
	}
	PackedByteArray page;
	uint32_t		instance_count = 0u;
	uint16_t		mesh_count = 0u;
	uint32_t		mesh_counts[T3D_MAX_MESH_ASSETS] = {};
	if ( !Terrain3DInstancePageCodec::encode( m_instanceStore, page, mesh_counts, instance_count, mesh_count ) ) {
		LOG( ERROR, "Failed encoding instance page for region ", m_location );
		return false;
	}
	m_instancePage = page;
	memcpy( m_szInstanceMeshCounts, mesh_counts, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = instance_count;
	m_nInstanceMeshCount = mesh_count;
	m_bInstanceStoreModified = false;
	return true;
}

void Terrain3DRegion::importInstancesUnmodified(
	const Dictionary &rInstances ) {
	m_instanceStore.importDictionary( rInstances );
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = true;
	m_instancePage.clear();
	m_cookedInstanceMetadata.clear();
	m_cookedMultimeshFastStart.clear();
	if ( !encodeInstancePage() ) {
		clearInstanceMetadata();
	}
	LOG( DEBUG, "Region ", m_location, " imported ", ( int ) ( m_nInstanceCount ), " native instances" );
}

void Terrain3DRegion::setInstances(
	const Dictionary &rInstances ) {
	if ( !ensureInstanceStoreResident() ) {
		LOG( ERROR, "Failed materializing instance store before setInstances for region ", m_location );
		return;
	}
	if ( m_instanceStore.equalsDictionary( rInstances ) ) {
		return;
	}
	importInstancesUnmodified( rInstances );
	markAuthoringInstancesChanged();
	m_bInstanceStoreModified = true;
	m_bInstancesDirty = true;
	m_bModified = true;
}

Dictionary Terrain3DRegion::getInstancesLegacyStorage(
	void ) const {
	// New saves use instance_page. Keep only the setter-backed legacy property so
	// old .res files still load without writing a duplicate Dictionary payload.
	return Dictionary();
}

Dictionary Terrain3DRegion::getInstances(
	void ) const {
	if ( !ensureInstanceStoreResident() ) {
		return Dictionary();
	}
	return m_instanceStore.exportDictionary();
}

void Terrain3DRegion::setInstancePage(
	const PackedByteArray &rPage ) {
	if ( rPage == m_instancePage && !m_bInstanceStoreModified ) {
		return;
	}
	uint32_t mesh_counts[T3D_MAX_MESH_ASSETS] = {};
	uint32_t instance_count = 0u;
	uint16_t mesh_count = 0u;
	if ( !Terrain3DInstancePageCodec::inspect( rPage, mesh_counts, instance_count, mesh_count ) ) {
		LOG( ERROR, "Rejected invalid instance page for region ", m_location );
		return;
	}
	m_instancePage = rPage;
	markAuthoringInstancesChanged();
	m_bInstancesDirty = true;
	m_cookedInstanceMetadata.clear();
	m_cookedMultimeshFastStart.clear();
	m_instanceStore.clear();
	m_bInstanceStoreResident = rPage.is_empty();
	m_bInstanceStoreModified = false;
	memcpy( m_szInstanceMeshCounts, mesh_counts, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = instance_count;
	m_nInstanceMeshCount = mesh_count;
}

PackedByteArray Terrain3DRegion::getInstancePage(
	void ) const {
	if ( !encodeInstancePage() ) {
		return PackedByteArray();
	}
	return m_instancePage;
}

bool Terrain3DRegion::ensureInstanceStoreResident(
	void ) const {
	if ( m_bInstanceStoreResident ) {
		return true;
	}
	Terrain3DInstanceStore decoded;
	uint32_t			   mesh_counts[T3D_MAX_MESH_ASSETS] = {};
	uint32_t			   instance_count = 0u;
	uint16_t			   mesh_count = 0u;
	if ( !Terrain3DInstancePageCodec::decode( m_instancePage, decoded, mesh_counts, instance_count, mesh_count ) ) {
		LOG( ERROR, "Failed decoding instance page for region ", m_location );
		return false;
	}
	m_instanceStore = eastl::move( decoded );
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = false;
	memcpy( m_szInstanceMeshCounts, mesh_counts, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = instance_count;
	m_nInstanceMeshCount = mesh_count;
	return true;
}

void Terrain3DRegion::adoptPreparedInstanceStoreInternal(
	Terrain3DInstanceStore &&rStore ) {
	m_instanceStore = eastl::move( rStore );
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = false;
}

void Terrain3DRegion::adoptCookedInstanceAccelerationInternal(
	const PackedByteArray &rMetadata,
	const PackedByteArray &rFastStart ) {
	m_cookedInstanceMetadata = rMetadata;
	m_cookedMultimeshFastStart = rFastStart;
}

void Terrain3DRegion::setCookedInstanceMetadataInternal(
	const uint32_t *const pMeshCounts,
	const uint32_t		  nInstanceCount,
	const uint16_t		  nMeshCount ) {
	if ( pMeshCounts ) {
		memcpy( m_szInstanceMeshCounts, pMeshCounts, sizeof( m_szInstanceMeshCounts ) );
	} else {
		memset( m_szInstanceMeshCounts, 0, sizeof( m_szInstanceMeshCounts ) );
	}
	m_nInstanceCount = nInstanceCount;
	m_nInstanceMeshCount = nMeshCount;
	m_instanceStore.clear();
	m_instancePage.clear();
	m_cookedInstanceMetadata.clear();
	m_cookedMultimeshFastStart.clear();
	m_bInstanceStoreResident = nInstanceCount == 0u;
	m_bInstanceStoreModified = false;
}

void Terrain3DRegion::setCookedStreamedNodeMetadataInternal(
	const uint16_t	nCount,
	const Vector2i &rCoverageMin,
	const Vector2i &rCoverageMax ) {
	m_nCookedStreamedNodeCount = nCount;
	m_cookedStreamedNodeCoverageMin = rCoverageMin;
	m_cookedStreamedNodeCoverageMax = rCoverageMax;
}

bool Terrain3DRegion::reserveStreamedNodeRuntimeCapacityInternal(
	const size_t nCapacity ) {
	return t3dContainerReserveCold( m_streamedNodeRuntime, nCapacity );
}

void Terrain3DRegion::adoptCookedStreamedNodeRuntimeInternal(
	eastl::vector< t3dStreamedNodeRuntimeDescriptor_t > &&rNodes ) {
	m_streamedNodeRuntime = eastl::move( rNodes );
	m_nCookedStreamedNodeCount = ( uint16_t ) ( MIN( m_streamedNodeRuntime.size(), ( size_t ) ( UINT16_MAX ) ) );
	refreshStreamedNodeCoverage();
	m_bModified = false;
}

bool Terrain3DRegion::evictInstanceStore(
	void ) {
	if ( !m_bInstanceStoreResident ) {
		return true;
	}
	if ( !encodeInstancePage() ) {
		return false;
	}
	Terrain3DInstanceStore empty;
	m_instanceStore = eastl::move( empty );
	m_cookedInstanceMetadata.clear();
	m_cookedMultimeshFastStart.clear();
	m_bInstanceStoreResident = false;
	return true;
}

bool Terrain3DRegion::releaseRuntimeAllocatorStorageInternal(
	void ) {
	if ( m_bInstanceStoreResident && !encodeInstancePage() ) {
		return false;
	}
	m_instanceStore.releaseStorage();
	m_bInstanceStoreResident = m_nInstanceCount == 0u;

	return true;
}

bool Terrain3DRegion::streamedNodeIdExists(
	const uint64_t nId ) const {
	if ( nId == 0u || nId > ( uint64_t ) ( INT64_MAX ) ) {
		return false;
	}
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		const Dictionary entry = m_streamedNodes[index];
		if ( entry.has( "id" ) && ( int64_t ) ( entry["id"] ) > 0 &&
			 ( uint64_t ) ( ( int64_t ) ( entry["id"] ) ) == nId ) {
			return true;
		}
	}
	return false;
}

int64_t Terrain3DRegion::allocateStreamedNodeId(
	const int64_t nRequestedId ) {
	if ( nRequestedId > 0 ) {
		const uint64_t requested = ( uint64_t ) ( nRequestedId );
		if ( streamedNodeIdExists( requested ) ) {
			return 0;
		}
		m_nNextStreamedNodeId = requested < ( uint64_t ) ( INT64_MAX ) ? requested + 1u : 1u;
		return nRequestedId;
	}
	const int attempts = m_streamedNodes.size() + 1;
	for ( int attempt = 0; attempt < attempts; ++attempt ) {
		if ( m_nNextStreamedNodeId == 0u || m_nNextStreamedNodeId > ( uint64_t ) ( INT64_MAX ) ) {
			m_nNextStreamedNodeId = 1u;
		}
		const uint64_t candidate = m_nNextStreamedNodeId++;
		if ( !streamedNodeIdExists( candidate ) ) {
			return ( int64_t ) ( candidate );
		}
	}
	return 0;
}

void Terrain3DRegion::refreshStreamedNodeId(
	void ) {
	uint64_t next_id = 1u;
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		const Dictionary entry = m_streamedNodes[index];
		if ( !entry.has( "id" ) ) {
			continue;
		}
		const int64_t id = ( int64_t ) ( entry["id"] );
		if ( id > 0 && ( uint64_t ) ( id ) >= next_id ) {
			next_id = ( uint64_t ) ( id ) < ( uint64_t ) ( INT64_MAX ) ? ( uint64_t ) ( id ) + 1u : 1u;
		}
	}
	m_nNextStreamedNodeId = next_id == 0u || next_id > ( uint64_t ) ( INT64_MAX ) ? 1u : next_id;
}

void Terrain3DRegion::rebuildStreamedNodeRuntime(
	void ) {
	m_streamedNodeRuntime.clear();
	m_streamedNodeRuntime.reserve( ( size_t ) ( m_streamedNodes.size() ) );
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		if ( Variant( m_streamedNodes[index] ).get_type() != Variant::DICTIONARY ) {
			continue;
		}
		const Dictionary entry = m_streamedNodes[index];
		if ( !entry.has( "id" ) || !entry.has( "transform" ) ) {
			continue;
		}
		t3dStreamedNodeRuntimeDescriptor_t descriptor;
		descriptor.m_nId = ( int64_t ) ( entry["id"] );
		descriptor.m_spawnName = StringName( String( "Spawn_" ) + String::num_int64( descriptor.m_nId ) );
		descriptor.m_scene = entry.has( "scene" ) ? Ref< PackedScene >( entry["scene"] ) : Ref< PackedScene >();
		descriptor.m_scenePath = entry.has( "scene_path" ) ? String( entry["scene_path"] ) : String();
		descriptor.m_transform = Transform3D( entry["transform"] );
		descriptor.m_state = entry.has( "state" ) && Variant( entry["state"] ).get_type() == Variant::DICTIONARY
								 ? Dictionary( entry["state"] )
								 : Dictionary();
		descriptor.m_nFlags = entry.has( "flags" ) ? ( int ) ( entry["flags"] ) : 0;
		descriptor.m_nHlodMinLevel = CLAMP( entry.has( "hlod_min_level" ) ? ( int ) entry["hlod_min_level"] : 1, 1, 6 );
		descriptor.m_nBoundaryPolicy =
			entry.has( "boundary_policy" )
				? t3dRegionNormalizeStreamedNodeBoundaryPolicy( ( int ) ( entry["boundary_policy"] ) )
				: STREAMED_NODE_BOUNDARY_ANCHOR;
		descriptor.m_coverageMin = entry.has( "coverage_min" ) ? Vector2i( entry["coverage_min"] ) : s_v2iZero;
		descriptor.m_coverageMax = entry.has( "coverage_max" ) ? Vector2i( entry["coverage_max"] ) : s_v2iZero;
		t3dRegionNormalizeStreamedNodeCoverage( descriptor.m_nBoundaryPolicy,
												descriptor.m_coverageMin,
												descriptor.m_coverageMax,
												descriptor.m_coverageMin,
												descriptor.m_coverageMax );
		descriptor.m_nLightingClass = t3dRegionNormalizeLightingClass(
			entry.has( "lighting_class" ) ? ( int ) ( entry["lighting_class"] )
										  : ( int ) ( LIGHTING_CLASS_MEDIUM_OBSTACLE ) );
		descriptor.m_nBakedLightingMode = t3dRegionNormalizeBakedLightingMode(
			entry.has( "baked_lighting_mode" ) ? ( int ) ( entry["baked_lighting_mode"] )
											   : ( int ) ( BAKED_LIGHTING_LIGHTMAP ) );
		descriptor.m_bContributeBakedGi =
			entry.has( "contribute_baked_gi" ) ? ( bool ) ( entry["contribute_baked_gi"] ) : true;
		descriptor.m_bReceiveDynamicDirectional =
			entry.has( "receive_dynamic_directional" ) ? ( bool ) ( entry["receive_dynamic_directional"] ) : true;
		descriptor.m_bDynamicShadowEligible =
			entry.has( "dynamic_shadow_eligible" ) ? ( bool ) ( entry["dynamic_shadow_eligible"] ) : true;
		descriptor.m_shadowProxyScenePath =
			entry.has( "shadow_proxy_scene_path" ) ? String( entry["shadow_proxy_scene_path"] ) : String();
		descriptor.m_shadowProxyScene = entry.has( "shadow_proxy_scene" )
											? Ref< PackedScene >( entry["shadow_proxy_scene"] )
											: Ref< PackedScene >();
		m_streamedNodeRuntime.push_back( eastl::move( descriptor ) );
	}
}

void Terrain3DRegion::refreshStreamedNodeCoverage(
	void ) {
	m_streamedNodeCoverageMin = s_v2iZero;
	m_streamedNodeCoverageMax = s_v2iZero;
	m_bStreamedNodeHasExtendedCoverage = false;
	for ( size_t iNode = 0u; iNode < ( size_t ) ( ( m_streamedNodeRuntime ).size() ); iNode++ ) {
		const t3dStreamedNodeRuntimeDescriptor_t &descriptor = ( m_streamedNodeRuntime )[iNode];
		if ( descriptor.m_nBoundaryPolicy != STREAMED_NODE_BOUNDARY_COVERAGE ) {
			continue;
		}
		const Vector2i coverage_min = descriptor.m_coverageMin;
		const Vector2i coverage_max = descriptor.m_coverageMax;
		m_streamedNodeCoverageMin.x = MIN( m_streamedNodeCoverageMin.x, coverage_min.x );
		m_streamedNodeCoverageMin.y = MIN( m_streamedNodeCoverageMin.y, coverage_min.y );
		m_streamedNodeCoverageMax.x = MAX( m_streamedNodeCoverageMax.x, coverage_max.x );
		m_streamedNodeCoverageMax.y = MAX( m_streamedNodeCoverageMax.y, coverage_max.y );
		if ( coverage_min != s_v2iZero || coverage_max != s_v2iZero ) {
			m_bStreamedNodeHasExtendedCoverage = true;
		}
	}
}

bool Terrain3DRegion::streamedNodeCoverageIntersects(
	const Vector2i &rOwnerLocation,
	const Vector2i &rTargetMin,
	const Vector2i &vTargetMax ) const {
	if ( m_streamedNodeRuntime.empty() ) {
		if ( m_nCookedStreamedNodeCount == 0u ) {
			return false;
		}
		const Vector2i cooked_min = rOwnerLocation + m_cookedStreamedNodeCoverageMin;
		const Vector2i cooked_max = rOwnerLocation + m_cookedStreamedNodeCoverageMax;
		return rOwnerLocation.x >= rTargetMin.x && rOwnerLocation.x <= vTargetMax.x &&
				   rOwnerLocation.y >= rTargetMin.y && rOwnerLocation.y <= vTargetMax.y ||
			   ( cooked_min.x <= vTargetMax.x && cooked_max.x >= rTargetMin.x && cooked_min.y <= vTargetMax.y &&
				 cooked_max.y >= rTargetMin.y );
	}
	if ( rOwnerLocation.x >= rTargetMin.x && rOwnerLocation.x <= vTargetMax.x && rOwnerLocation.y >= rTargetMin.y &&
		 rOwnerLocation.y <= vTargetMax.y ) {
		return true;
	}
	if ( !m_bStreamedNodeHasExtendedCoverage ) {
		return false;
	}
	const Vector2i aggregate_min = rOwnerLocation + m_streamedNodeCoverageMin;
	const Vector2i aggregate_max = rOwnerLocation + m_streamedNodeCoverageMax;
	if ( aggregate_min.x > vTargetMax.x || aggregate_max.x < rTargetMin.x || aggregate_min.y > vTargetMax.y ||
		 aggregate_max.y < rTargetMin.y ) {
		return false;
	}
	for ( size_t iNode = 0u; iNode < ( size_t ) ( ( m_streamedNodeRuntime ).size() ); iNode++ ) {
		const t3dStreamedNodeRuntimeDescriptor_t &descriptor = ( m_streamedNodeRuntime )[iNode];
		if ( descriptor.m_nBoundaryPolicy != STREAMED_NODE_BOUNDARY_COVERAGE ) {
			continue;
		}
		const Vector2i coverage_min = rOwnerLocation + descriptor.m_coverageMin;
		const Vector2i coverage_max = rOwnerLocation + descriptor.m_coverageMax;
		if ( coverage_min.x <= vTargetMax.x && coverage_max.x >= rTargetMin.x && coverage_min.y <= vTargetMax.y &&
			 coverage_max.y >= rTargetMin.y ) {
			return true;
		}
	}
	return false;
}

const t3dStreamedNodeRuntimeDescriptor_t *Terrain3DRegion::findStreamedNodeRuntimeInternal(
	const int64_t nId ) const {
	for ( size_t iNode = 0u; iNode < ( size_t ) ( ( m_streamedNodeRuntime ).size() ); iNode++ ) {
		const t3dStreamedNodeRuntimeDescriptor_t &descriptor = ( m_streamedNodeRuntime )[iNode];
		if ( descriptor.m_nId == nId ) {
			return &descriptor;
		}
	}
	return NULL;
}

void Terrain3DRegion::setStreamedNodes(
	const Array &rNodes ) {
	m_nCookedStreamedNodeCount = 0u;
	m_streamedNodes.clear();
	m_nNextStreamedNodeId = 1u;
	for ( int index = 0; index < rNodes.size(); ++index ) {
		if ( Variant( rNodes[index] ).get_type() != Variant::DICTIONARY ) {
			continue;
		}
		const Dictionary source = rNodes[index];
		if ( !source.has( "transform" ) ) {
			continue;
		}
		const String			 scene_path = source.has( "scene_path" ) ? String( source["scene_path"] ) : String();
		const Ref< PackedScene > pScene =
			source.has( "scene" ) ? Ref< PackedScene >( source["scene"] ) : Ref< PackedScene >();
		if ( scene_path.is_empty() && pScene.is_null() ) {
			continue;
		}
		const int64_t requested_id = source.has( "id" ) ? ( int64_t ) ( source["id"] ) : 0;
		const int64_t id = allocateStreamedNodeId( requested_id );
		if ( id <= 0 ) {
			LOG( WARN, "Skipped duplicate streamed-node id ", requested_id, " in region ", m_location );
			continue;
		}
		Dictionary entry;
		entry["id"] = id;
		if ( !scene_path.is_empty() ) {
			entry["scene_path"] = scene_path;
		} else {
			entry["scene"] = pScene;
		}
		entry["transform"] = Transform3D( source["transform"] );
		entry["state"] = source.has( "state" ) && Variant( source["state"] ).get_type() == Variant::DICTIONARY
							 ? Dictionary( source["state"] ).duplicate( true )
							 : Dictionary();
		entry["flags"] = source.has( "flags" ) ? ( int ) ( source["flags"] ) : 0;
		entry["hlod_min_level"] = CLAMP( source.has( "hlod_min_level" ) ? ( int ) source["hlod_min_level"] : 1, 1, 6 );
		t3dRegionStoreStreamedNodeBoundary(
			entry,
			source.has( "boundary_policy" ) ? ( int ) ( source["boundary_policy"] ) : STREAMED_NODE_BOUNDARY_ANCHOR,
			source.has( "coverage_min" ) ? Vector2i( source["coverage_min"] ) : s_v2iZero,
			source.has( "coverage_max" ) ? Vector2i( source["coverage_max"] ) : s_v2iZero );
		t3dRegionStoreStreamedNodeLighting( entry, source );
		m_streamedNodes.push_back( entry );
	}
	refreshStreamedNodeId();
	rebuildStreamedNodeRuntime();
	refreshStreamedNodeCoverage();
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
}

int64_t Terrain3DRegion::addStreamedNode(
	const Ref< PackedScene > &rScene,
	const Transform3D		 &rLocalTransform,
	const Dictionary		 &rState,
	const int				  nFlags,
	const int64_t			  nId,
	const int				  nBoundaryPolicy,
	const Vector2i			 &rCoverageMin,
	const Vector2i			 &rCoverageMax,
	const int				  nHlodMinLevel ) {
	if ( rScene.is_null() ) {
		return 0;
	}
	const int64_t id = allocateStreamedNodeId( nId );
	if ( id <= 0 ) {
		return 0;
	}
	Dictionary entry;
	entry["id"] = id;
	entry["scene"] = rScene;
	entry["transform"] = rLocalTransform;
	entry["state"] = rState.duplicate( true );
	entry["flags"] = nFlags;
	entry["hlod_min_level"] = CLAMP( nHlodMinLevel, 1, 6 );
	t3dRegionStoreStreamedNodeBoundary( entry, nBoundaryPolicy, rCoverageMin, rCoverageMax );
	t3dRegionStoreStreamedNodeLighting( entry, Dictionary() );
	m_streamedNodes.push_back( entry );
	rebuildStreamedNodeRuntime();
	refreshStreamedNodeCoverage();
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
	return id;
}

int64_t Terrain3DRegion::addStreamedNodePath(
	const String	  &rScenePath,
	const Transform3D &rLocalTransform,
	const Dictionary  &rState,
	const int		   nFlags,
	const int64_t	   nId,
	const int		   nBoundaryPolicy,
	const Vector2i	  &rCoverageMin,
	const Vector2i	  &rCoverageMax,
	const int		   nHlodMinLevel ) {
	if ( rScenePath.is_empty() ) {
		return 0;
	}
	const int64_t id = allocateStreamedNodeId( nId );
	if ( id <= 0 ) {
		return 0;
	}
	Dictionary entry;
	entry["id"] = id;
	entry["scene_path"] = rScenePath;
	entry["transform"] = rLocalTransform;
	entry["state"] = rState.duplicate( true );
	entry["flags"] = nFlags;
	entry["hlod_min_level"] = CLAMP( nHlodMinLevel, 1, 6 );
	t3dRegionStoreStreamedNodeBoundary( entry, nBoundaryPolicy, rCoverageMin, rCoverageMax );
	t3dRegionStoreStreamedNodeLighting( entry, Dictionary() );
	m_streamedNodes.push_back( entry );
	rebuildStreamedNodeRuntime();
	refreshStreamedNodeCoverage();
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
	return id;
}

bool Terrain3DRegion::removeStreamedNode(
	const int64_t nId ) {
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		const Dictionary entry = m_streamedNodes[index];
		if ( entry.has( "id" ) && ( int64_t ) ( entry["id"] ) == nId ) {
			m_streamedNodes.remove_at( index );
			rebuildStreamedNodeRuntime();
			refreshStreamedNodeCoverage();
			m_bMetadataDirty = true; markAuthoringSourceChanged();
			m_bModified = true;
			return true;
		}
	}
	return false;
}

bool Terrain3DRegion::updateStreamedNodeState(
	const int64_t	   nId,
	const Transform3D &rLocalTransform,
	const Dictionary  &rState,
	const int		   nFlags ) {
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		Dictionary entry = m_streamedNodes[index];
		if ( !entry.has( "id" ) || ( int64_t ) ( entry["id"] ) != nId ) {
			continue;
		}
		const Transform3D old_transform = entry.has( "transform" ) ? Transform3D( entry["transform"] ) : Transform3D();
		const Dictionary  old_state = entry.has( "state" ) ? Dictionary( entry["state"] ) : Dictionary();
		const int		  old_flags = entry.has( "flags" ) ? ( int ) ( entry["flags"] ) : 0;
		if ( old_transform.is_equal_approx( rLocalTransform ) && old_state == rState && old_flags == nFlags ) {
			return true;
		}
		entry["transform"] = rLocalTransform;
		entry["state"] = rState.duplicate( true );
		entry["flags"] = nFlags;
		m_streamedNodes[index] = entry;
		rebuildStreamedNodeRuntime();
		m_bMetadataDirty = true; markAuthoringSourceChanged();
		m_bModified = true;
		return true;
	}
	return false;
}

void Terrain3DRegion::setLighting(
	const Ref< Terrain3DRegionLighting > &rLighting ) {
	if ( m_lighting == rLighting ) {
		return;
	}
	m_lighting = rLighting;
	Terrain3DRegionLighting::markGlobalPublicationDirtyInternal();
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
}

void Terrain3DRegion::setLocation(
	const Vector2i &rLocation ) {
	// Locations are bounded by the fixed logical region map.
	if ( Terrain3DData::getRegionMapIndex( rLocation ) < 0 ) {
		LOG( ERROR,
			 "Location ",
			 rLocation,
			 " out of bounds. Max: ",
			 -T3D_REGION_MAP_SIZE / 2,
			 " to ",
			 T3D_REGION_MAP_SIZE / 2 - 1 );
		return;
	}
	// Marked modified if setting after initialized
	if ( m_location < s_v2iMax && m_location != rLocation ) {
		m_bMetadataDirty = true; markAuthoringSourceChanged();
		m_bModified = true;
	}
	SET_IF_DIFF( m_location, rLocation );
	LOG( DEBUG, "Set location: ", rLocation );
}

Error Terrain3DRegion::save(
	const String &rPath,
	const bool	  b16Bit ) {
	compactDefaultMaps();
	if ( m_bInstanceStoreResident ) {
		m_instanceStore.compactStorage();
	}
	if ( !encodeInstancePage() ) {
		return ERR_INVALID_DATA;
	}
	// Initiate save to external file. The scene will save itself.
	if ( m_location.x == INT32_MAX ) {
		LOG( ERROR, "Region has not been setup. Location is INT32_MAX. Skipping ", rPath );
	}
	if ( !m_bModified ) {
		LOG( DEBUG, "Region ", m_location, " not modified. Skipping ", rPath );
		return ERR_SKIP;
	}
	if ( m_heightMap.is_null() ) {
		LOG( ERROR,
			 "Cannot save mapless region ",
			 m_location,
			 ". Page it through Terrain3DData::ensureCpuRegionResident() before direct region save." );
		return ERR_UNAVAILABLE;
	}
	if ( rPath.is_empty() && get_path().is_empty() ) {
		LOG( ERROR, "No valid path provided" );
		return ERR_FILE_NOT_FOUND;
	}
	if ( !rPath.is_empty() ) {
		LOG( DEBUG, "Setting file path for region ", m_location, " to ", rPath );
		take_over_path( rPath );
		// Set region path and take over the path from any other cached resources,
		// incuding those in the undo queue
	}
	LOG( MESG, "Writing compact R16 region ", m_location, " to ", get_path() );
	setVersion( s_fDataCurrentVersion );
	( void ) b16Bit;
    const uint64_t nCompressionStarted = Time::get_singleton()->get_ticks_usec();
    LOG( INFO, "[Terrain3D][Save][Region][CompressedResource] BEGIN location=", m_location,
        " path=", get_path(), " flags=FLAG_COMPRESS" );
	const Error err = ResourceSaver::get_singleton()->save( this, get_path(), ResourceSaver::FLAG_COMPRESS );
    LOG( err == OK ? INFO : ERROR,
        "[Terrain3D][Save][Region][CompressedResource] END location=", m_location,
        " error=", err, " elapsed_us=", Time::get_singleton()->get_ticks_usec() - nCompressionStarted );
	if ( err == OK ) {
		m_bModified = false;
		m_bInstancesDirty = false;
		m_bMetadataDirty = false;
		m_bRuntimeMapsModified = false;
		m_nRasterDirtyDomains = 0u;
		LOG( INFO, "File saved successfully" );
	} else {
		LOG( ERROR,
			 "Cannot save region file: ",
			 get_path(),
			 ". Error code: ",
			 ERROR,
			 ". Look up @GlobalScope Error enum in the Godot docs" );
	}
	return err;
}

void Terrain3DRegion::setData(
	const Dictionary &rData ) {
	// This is a public mutation API, not a deserializer. Route through validated
	// setters and publish one conservative dirty result instead of copying raw
	// internal fields with no downstream contract.
	if ( rData.has( "region_size" ) && ( int ) ( rData["region_size"] ) != T3D_REGION_SIZE ) {
		m_bGeometryContractValid = false;
		LOG( ERROR, "Rejected region data with legacy geometry: region_size must be exactly 128 samples" );
		return;
	}
	if ( rData.has( "vertex_spacing" ) &&
		 ( real_t ) ( rData["vertex_spacing"] ) != ( real_t ) ( s_fRegionVertexSpacing ) ) {
		m_bGeometryContractValid = false;
		LOG( ERROR, "Rejected region data with legacy geometry: vertex_spacing must be exactly 1.0 metre" );
		return;
	}
	if ( rData.has( "version" ) ) {
		setVersion( ( real_t ) ( rData["version"] ) );
	}
	if ( rData.has( "location" ) ) {
		setLocation( Vector2i( rData["location"] ) );
	}
	if ( rData.has( "height_map" ) ) {
		setHeightMap( rData["height_map"] );
	}
	if ( rData.has( "control_map" ) ) {
		setControlMap( rData["control_map"] );
	}
	if ( rData.has( "color_map" ) ) {
		setColorMap( rData["color_map"] );
	}
	if ( rData.has( "channel_flags" ) ) {
		setChannelFlags( ( int ) ( rData["channel_flags"] ) );
	}
	if ( rData.has( "megavt_source_generation" ) ) {
		setMegaVTSourceGeneration( ( int64_t ) ( rData["megavt_source_generation"] ) );
	}
	if ( rData.has( "instance_generation" ) ) {
		setInstanceGeneration( ( int64_t ) ( rData["instance_generation"] ) );
	}
	if ( rData.has( "megavt_dirty_mip0_mask" ) ) {
		setMegaVTDirtyMip0Mask( ( int64_t ) ( rData["megavt_dirty_mip0_mask"] ) );
	}
	if ( rData.has( "instances" ) ) {
		setInstances( rData["instances"] );
	}
	if ( rData.has( "instance_page" ) ) {
		setInstancePage( rData["instance_page"] );
	}
	if ( rData.has( "streamed_nodes" ) ) {
		setStreamedNodes( rData["streamed_nodes"] );
	}
	if ( rData.has( "lighting" ) ) {
		setLighting( rData["lighting"] );
	}
	if ( rData.has( "deleted" ) ) {
		setDeleted( ( bool ) ( rData["deleted"] ) );
	}
	if ( rData.has( "edited" ) ) {
		setEdited( ( bool ) ( rData["edited"] ) );
	}
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
}

Dictionary Terrain3DRegion::getData(
	void ) const {
	Dictionary dict;
	dict["location"] = m_location;
	dict["deleted"] = m_bDeleted;
	dict["edited"] = m_bEdited;
	dict["modified"] = m_bModified;
	dict["version"] = m_fVersion;
	dict["height_range"] = m_heightRange;
	dict["height_map"] = m_heightMap;
	dict["control_map"] = m_controlMap;
	dict["color_map"] = m_colorMap;
	dict["channel_flags"] = ( int ) ( m_nChannelFlags );
	dict["megavt_source_generation"] = ( int64_t ) ( m_nMegaVTSourceGeneration );
	dict["instance_generation"] = m_nInstanceGeneration;
	dict["megavt_dirty_mip0_mask"] = ( int64_t ) ( m_nMegaVTDirtyMip0Mask );
	dict["instances"] = Dictionary();
	dict["instance_page"] = getInstancePage();
	dict["streamed_nodes"] = m_streamedNodes.duplicate( true );
	dict["lighting"] = m_lighting;
	return dict;
}

Ref< Terrain3DRegion > Terrain3DRegion::duplicate(
	const bool bDeep ) {
	Ref< Terrain3DRegion > pRegion;
	pRegion.instantiate();
	pRegion->m_fVersion = m_fVersion;
	pRegion->m_bGeometryContractValid = m_bGeometryContractValid;
	pRegion->m_heightRange = m_heightRange;
	pRegion->m_nChannelFlags = m_nChannelFlags;
	pRegion->m_nMegaVTSourceGeneration = m_nMegaVTSourceGeneration;
	pRegion->m_nInstanceGeneration = m_nInstanceGeneration;
#if defined(T3D_EDITOR_NATIVE)
	pRegion->m_editorRevisions = m_editorRevisions;
#endif
	pRegion->m_nRasterDirtyDomains = m_nRasterDirtyDomains;
	pRegion->m_bInstancesDirty = m_bInstancesDirty;
	pRegion->m_bMetadataDirty = m_bMetadataDirty;
	pRegion->m_bRuntimeMapsModified = m_bRuntimeMapsModified;
	pRegion->m_nMegaVTDirtyMip0Mask = m_nMegaVTDirtyMip0Mask;
	pRegion->m_bModified = m_bModified;
	pRegion->m_bDeleted = m_bDeleted;
	pRegion->m_location = m_location;
	pRegion->m_instanceStore = m_instanceStore;
	pRegion->m_instancePage = getInstancePage();
	pRegion->m_bInstanceStoreResident = m_bInstanceStoreResident;
	pRegion->m_bInstanceStoreModified = m_bInstanceStoreModified;
	memcpy( pRegion->m_szInstanceMeshCounts, m_szInstanceMeshCounts, sizeof( m_szInstanceMeshCounts ) );
	pRegion->m_nInstanceCount = m_nInstanceCount;
	pRegion->m_nInstanceMeshCount = m_nInstanceMeshCount;
	pRegion->m_streamedNodes = m_streamedNodes.duplicate( true );
	pRegion->rebuildStreamedNodeRuntime();
	pRegion->m_nNextStreamedNodeId = m_nNextStreamedNodeId;
	pRegion->m_streamedNodeCoverageMin = m_streamedNodeCoverageMin;
	pRegion->m_streamedNodeCoverageMax = m_streamedNodeCoverageMax;
	pRegion->m_bStreamedNodeHasExtendedCoverage = m_bStreamedNodeHasExtendedCoverage;
	pRegion->m_lighting = m_lighting;
	if ( bDeep ) {
		pRegion->m_heightMap = m_heightMap.is_valid() ? Ref< Image >( m_heightMap->duplicate() ) : Ref< Image >();
		pRegion->m_controlMap = m_controlMap.is_valid() ? Ref< Image >( m_controlMap->duplicate() ) : Ref< Image >();
		pRegion->m_colorMap = m_colorMap.is_valid() ? Ref< Image >( m_colorMap->duplicate() ) : Ref< Image >();
	} else {
		pRegion->m_bEdited = m_bEdited;
		pRegion->m_heightMap = m_heightMap;
		pRegion->m_controlMap = m_controlMap;
		pRegion->m_colorMap = m_colorMap;
	}
	return pRegion;
}

void Terrain3DRegion::dump(
	const bool bVerbose ) const {
	LOG( MESG,
		 "Region: ",
		 m_location,
		 ", version: ",
		 vformat( "%.2f", m_fVersion ),
		 ", size: ",
		 T3D_REGION_SIZE,
		 ", spacing: ",
		 vformat( "%.1f", ( real_t ) ( s_fRegionVertexSpacing ) ),
		 ", range: ",
		 vformat( "%.2v", m_heightRange ),
		 ", flags (",
		 m_bEdited ? "ed," : "",
		 m_bModified ? "mod," : "",
		 m_bDeleted ? "del" : "",
		 ")" );
	LOG( MESG,
		 "Instances: mesh groups: ",
		 ( int ) ( m_nInstanceMeshCount ),
		 ", total: ",
		 ( int ) ( m_nInstanceCount ),
		 m_bInstanceStoreResident ? ", resident" : ", paged",
		 ", page bytes: ",
		 m_instancePage.size() );
	if ( !m_bInstanceStoreResident ) {
		LOG( MESG, "Streamed dynamic nodes: ", m_streamedNodes.size() );
		return;
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t ) ( ( m_instanceStore.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( m_instanceStore.meshes() )[iMesh];
		size_t					 count = 0;
		for ( size_t iCell = 0u; iCell < ( size_t ) ( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &cell = ( mesh.m_cells )[iCell];
			count += cell.m_transforms.size();
			if ( bVerbose ) {
				LOG( MESG,
					 "Mesh ID: ",
					 mesh.m_nId,
					 " cell: ",
					 cell.m_location,
					 " xforms: ",
					 ( int ) ( cell.m_transforms.size() ),
					 cell.usesColors() ? ", colored" : ", uncolored",
					 cell.m_bModified ? ", modified" : "" );
			}
		}
		LOG( MESG, "Mesh ID: ", mesh.m_nId, ", instance count: ", ( int ) ( count ) );
	}
}

// -----------------------------------------------------------------------------
// Protected functions

bool Terrain3DRegion::_set(
	const StringName &rName,
	const Variant	 &rProperty ) {
	if ( rName == StringName( "region_size" ) ) {
		m_bGeometryContractValid = m_bGeometryContractValid && ( int ) ( rProperty ) == T3D_REGION_SIZE;
		if ( !m_bGeometryContractValid ) {
			LOG( ERROR, "Rejected legacy Terrain3DRegion resource: region_size must be exactly 128 samples" );
		}
		return true;
	}
	if ( rName == StringName( "vertex_spacing" ) ) {
		m_bGeometryContractValid =
			m_bGeometryContractValid && ( real_t ) ( rProperty ) == ( real_t ) ( s_fRegionVertexSpacing );
		if ( !m_bGeometryContractValid ) {
			LOG( ERROR, "Rejected legacy Terrain3DRegion resource: vertex_spacing must be exactly 1.0 metre" );
		}
		return true;
	}
	return false;
}

void Terrain3DRegion::_bind_methods(
	void ) {
	BIND_ENUM_CONSTANT( LIGHTING_CLASS_TERRAIN );
	BIND_ENUM_CONSTANT( LIGHTING_CLASS_FOLIAGE_CLUTTER );
	BIND_ENUM_CONSTANT( LIGHTING_CLASS_MEDIUM_OBSTACLE );
	BIND_ENUM_CONSTANT( LIGHTING_CLASS_LARGE_OCCLUDER );
	BIND_ENUM_CONSTANT( LIGHTING_CLASS_REGIONAL_OCCLUDER );
	BIND_ENUM_CONSTANT( BAKED_LIGHTING_DISABLED );
	BIND_ENUM_CONSTANT( BAKED_LIGHTING_LIGHTMAP );
	BIND_ENUM_CONSTANT( BAKED_LIGHTING_PROBE );
	BIND_ENUM_CONSTANT( BAKED_LIGHTING_MATERIAL_ONLY );
	BIND_ENUM_CONSTANT( TYPE_HEIGHT );
	BIND_ENUM_CONSTANT( TYPE_CONTROL );
	BIND_ENUM_CONSTANT( TYPE_COLOR );
	BIND_ENUM_CONSTANT( REGION_TYPE_MAX );
	BIND_ENUM_CONSTANT( REGION_CHANNEL_CONTROL );
	BIND_ENUM_CONSTANT( REGION_CHANNEL_COLOR );
	BIND_ENUM_CONSTANT( REGION_CHANNEL_DERIVED );
	BIND_ENUM_CONSTANT( REGION_CHANNEL_MASK );
	BIND_ENUM_CONSTANT( STREAMED_NODE_BOUNDARY_ANCHOR );
	BIND_ENUM_CONSTANT( STREAMED_NODE_BOUNDARY_COVERAGE );

	ClassDB::bind_method( D_METHOD( "clear" ), &Terrain3DRegion::clear );
	ClassDB::bind_method( D_METHOD( "get_region_size" ), &Terrain3DRegion::getRegionSize );
	ClassDB::bind_method( D_METHOD( "get_vertex_spacing" ), &Terrain3DRegion::getVertexSpacing );

	ClassDB::bind_method( D_METHOD( "set_map", "map_type", "map" ), &Terrain3DRegion::setMap );
	ClassDB::bind_method( D_METHOD( "get_map", "map_type" ), &Terrain3DRegion::getMap );
	ClassDB::bind_method( D_METHOD( "set_maps", "maps" ), &Terrain3DRegion::setMaps );
	ClassDB::bind_method( D_METHOD( "get_maps" ), &Terrain3DRegion::getMaps );

	ClassDB::bind_method( D_METHOD( "has_channel", "map_type" ), &Terrain3DRegion::hasChannel );
	ClassDB::bind_method( D_METHOD( "sanitize_maps" ), &Terrain3DRegion::sanitizeMaps );
	ClassDB::bind_method( D_METHOD( "is_map_default", "map_type" ), &Terrain3DRegion::isMapDefault );
	ClassDB::bind_method( D_METHOD( "compact_map_if_default", "map_type", "mark_modified" ),
						  &Terrain3DRegion::compactMapIfDefault,
						  DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "compact_default_maps" ), &Terrain3DRegion::compactDefaultMaps );
	ClassDB::bind_method( D_METHOD( "mark_map_edited", "map_type", "recalculate_height_range" ),
						  &Terrain3DRegion::markMapEdited,
						  DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "sanitize_map", "map_type", "map" ), &Terrain3DRegion::sanitizeMap );
	ClassDB::bind_method( D_METHOD( "validate_map_size", "map" ), &Terrain3DRegion::validateMapSize );

	ClassDB::bind_method( D_METHOD( "update_height", "height" ), &Terrain3DRegion::updateHeight );
	ClassDB::bind_method( D_METHOD( "update_heights", "low_high" ), &Terrain3DRegion::updateHeights );
	ClassDB::bind_method( D_METHOD( "calc_height_range" ), &Terrain3DRegion::calcHeightRange );

	ClassDB::bind_method( D_METHOD( "set_instances", "instances" ), &Terrain3DRegion::setInstances );
	ClassDB::bind_method( D_METHOD( "get_instances" ), &Terrain3DRegion::getInstances );
	ClassDB::bind_method( D_METHOD( "_get_instances_legacy_storage" ), &Terrain3DRegion::getInstancesLegacyStorage );
	ClassDB::bind_method( D_METHOD( "ensure_instance_store_resident" ), &Terrain3DRegion::ensureInstanceStoreResident );
	ClassDB::bind_method( D_METHOD( "evict_instance_store" ), &Terrain3DRegion::evictInstanceStore );
	ClassDB::bind_method( D_METHOD( "is_instance_store_resident" ), &Terrain3DRegion::isInstanceStoreResident );
	ClassDB::bind_method( D_METHOD( "get_instance_count", "mesh_id" ),
						  &Terrain3DRegion::getInstanceCount,
						  DEFVAL( -1 ) );
	ClassDB::bind_method( D_METHOD( "get_instance_page_bytes" ), &Terrain3DRegion::getInstancePageBytes );
	ClassDB::bind_method( D_METHOD( "add_streamed_node",
									"scene",
									"local_transform",
									"state",
									"flags",
									"id",
									"boundary_policy",
									"coverage_min",
									"coverage_max",
									"hlod_min_level" ),
						  &Terrain3DRegion::addStreamedNode,
						  DEFVAL( Dictionary() ),
						  DEFVAL( 0 ),
						  DEFVAL( 0 ),
						  DEFVAL( STREAMED_NODE_BOUNDARY_ANCHOR ),
						  DEFVAL( s_v2iZero ),
						  DEFVAL( s_v2iZero ),
						  DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "add_streamed_node_path",
									"scene_path",
									"local_transform",
									"state",
									"flags",
									"id",
									"boundary_policy",
									"coverage_min",
									"coverage_max",
									"hlod_min_level" ),
						  &Terrain3DRegion::addStreamedNodePath,
						  DEFVAL( Dictionary() ),
						  DEFVAL( 0 ),
						  DEFVAL( 0 ),
						  DEFVAL( STREAMED_NODE_BOUNDARY_ANCHOR ),
						  DEFVAL( s_v2iZero ),
						  DEFVAL( s_v2iZero ),
						  DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "remove_streamed_node", "id" ), &Terrain3DRegion::removeStreamedNode );
	ClassDB::bind_method( D_METHOD( "get_streamed_node_count" ), &Terrain3DRegion::getStreamedNodeCount );
	ClassDB::bind_method( D_METHOD( "has_lighting" ), &Terrain3DRegion::hasLighting );
	ClassDB::bind_method( D_METHOD( "streamed_node_coverage_intersects", "owner_location", "target_min", "target_max" ),
						  &Terrain3DRegion::streamedNodeCoverageIntersects );
	ClassDB::bind_method( D_METHOD( "has_extended_streamed_node_coverage" ),
						  &Terrain3DRegion::hasExtendedStreamedNodeCoverage );

	ClassDB::bind_method( D_METHOD( "save", "path", "save_16_bit" ),
						  &Terrain3DRegion::save,
						  DEFVAL( "" ),
						  DEFVAL( false ) );

	ClassDB::bind_method( D_METHOD( "get_data" ), &Terrain3DRegion::getData );
	// Resource already binds duplicate(); expose the region-specific snapshot operation separately.
	ClassDB::bind_method( D_METHOD( "duplicate_region", "deep" ), &Terrain3DRegion::duplicate, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "dump", "verbose" ), &Terrain3DRegion::dump, DEFVAL( false ) );

	const int ro_flags = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ClassDB::bind_method( D_METHOD( "get_instance_generation" ), &Terrain3DRegion::getInstanceGeneration );
	ClassDB::bind_method( D_METHOD( "get_dirty_domains" ), &Terrain3DRegion::getDirtyDomains );
	ClassDB::bind_method( D_METHOD( "acknowledge_instance_save" ), &Terrain3DRegion::acknowledgeInstanceSave );
	ClassDB::bind_method( D_METHOD( "restore_instance_persistence_state", "dirty_domains", "modified" ),
		&Terrain3DRegion::restoreInstancePersistenceState );
	ClassDB::bind_method( D_METHOD( "set_instance_generation", "generation" ), &Terrain3DRegion::setInstanceGeneration );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_generation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_instance_generation", "get_instance_generation" );
	ADD_GROUP( "Identity / Format", "" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setVersion,
					   getVersion,
					   "version",
					   PropertyInfo( Variant::FLOAT, "version", PROPERTY_HINT_NONE, "", ro_flags ),
					   "set_version",
					   "get_version" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setHeightRange,
					   getHeightRange,
					   "range",
					   PropertyInfo( Variant::VECTOR2, "height_range", PROPERTY_HINT_NONE, "", ro_flags ),
					   "set_height_range",
					   "get_height_range" );
	T3D_BIND_PROPERTY(
		Terrain3DRegion,
		setChannelFlags,
		getChannelFlags,
		"flags",
		PropertyInfo( Variant::INT, "channel_flags", PROPERTY_HINT_FLAGS, "Control,Color,Derived", ro_flags ),
		"set_channel_flags",
		"get_channel_flags" );
	T3D_BIND_PROPERTY(
		Terrain3DRegion,
		setMegaVTSourceGeneration,
		getMegaVTSourceGeneration,
		"generation",
		PropertyInfo( Variant::INT, "megavt_source_generation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_megavt_source_generation",
		"get_megavt_source_generation" );
	T3D_BIND_PROPERTY(
		Terrain3DRegion,
		setMegaVTDirtyMip0Mask,
		getMegaVTDirtyMip0Mask,
		"mask",
		PropertyInfo( Variant::INT, "megavt_dirty_mip0_mask", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_megavt_dirty_mip0_mask",
		"get_megavt_dirty_mip0_mask" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setLocation,
					   getLocation,
					   "location",
					   PropertyInfo( Variant::VECTOR2I,
									 "location",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_location",
					   "get_location" );

	ADD_GROUP( "Maps", "" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setHeightMap,
					   getHeightMap,
					   "map",
					   PropertyInfo( Variant::OBJECT, "height_map", PROPERTY_HINT_RESOURCE_TYPE, "Image", ro_flags ),
					   "set_height_map",
					   "get_height_map" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setControlMap,
					   getControlMap,
					   "map",
					   PropertyInfo( Variant::OBJECT, "control_map", PROPERTY_HINT_RESOURCE_TYPE, "Image", ro_flags ),
					   "set_control_map",
					   "get_control_map" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setColorMap,
					   getColorMap,
					   "map",
					   PropertyInfo( Variant::OBJECT, "color_map", PROPERTY_HINT_RESOURCE_TYPE, "Image", ro_flags ),
					   "set_color_map",
					   "get_color_map" );

	ADD_GROUP( "Instances", "" );
	ADD_PROPERTY( PropertyInfo( Variant::DICTIONARY, "instances", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
				  "set_instances",
				  "_get_instances_legacy_storage" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setInstancePage,
					   getInstancePage,
					   "page",
					   PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instance_page", PROPERTY_HINT_NONE, "", ro_flags ),
					   "set_instance_page",
					   "get_instance_page" );

	ADD_GROUP( "Streamed Nodes", "" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setStreamedNodes,
					   getStreamedNodes,
					   "nodes",
					   PropertyInfo( Variant::ARRAY, "streamed_nodes", PROPERTY_HINT_NONE, "", ro_flags ),
					   "set_streamed_nodes",
					   "get_streamed_nodes" );

	ADD_GROUP( "Baked Lighting", "" );
	T3D_BIND_PROPERTY(
		Terrain3DRegion,
		setLighting,
		getLighting,
		"lighting",
		PropertyInfo( Variant::OBJECT, "lighting", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DRegionLighting", ro_flags ),
		"set_lighting",
		"get_lighting" );

	ADD_GROUP( "Runtime / Edit State", "" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setEdited,
					   isEdited,
					   "edited",
					   PropertyInfo( Variant::BOOL,
									 "edited",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_edited",
					   "is_edited" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setDeleted,
					   isDeleted,
					   "deleted",
					   PropertyInfo( Variant::BOOL,
									 "deleted",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_deleted",
					   "is_deleted" );
	T3D_BIND_PROPERTY( Terrain3DRegion,
					   setModified,
					   isModified,
					   "modified",
					   PropertyInfo( Variant::BOOL,
									 "modified",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_modified",
					   "is_modified" );
}

// M39 extracted non-template header implementations.
bool Terrain3DRegion::tryGetInstanceStore(
	const Terrain3DInstanceStore *&pStore ) const {
	pStore = NULL;
	if ( !ensureInstanceStoreResident() ) {
		return false;
	}
	pStore = &m_instanceStore;
	return true;
}

bool Terrain3DRegion::tryGetInstanceStoreMut(
	Terrain3DInstanceStore *&pStore ) {
	pStore = NULL;
	if ( !ensureInstanceStoreResident() ) {
		return false;
	}
	m_bMetadataDirty = true; markAuthoringSourceChanged();
	m_bModified = true;
	m_bInstanceStoreModified = true;
	pStore = &m_instanceStore;
	return true;
}

bool Terrain3DRegion::tryGetInstanceStoreInternal(
	Terrain3DInstanceStore *&pStore ) {
	pStore = NULL;
	if ( !ensureInstanceStoreResident() ) {
		return false;
	}
	pStore = &m_instanceStore;
	return true;
}

const Terrain3DInstanceStore &Terrain3DRegion::getInstanceStore(
	void ) const {
	const Terrain3DInstanceStore *pStore = NULL;
	if ( !tryGetInstanceStore( pStore ) || !pStore ) {
		abort();
	}
	return *pStore;
}

void Terrain3DRegion::markInstanceStoreModifiedInternal(
	void ) {
	markAuthoringInstancesChanged();
	m_bInstancesDirty = true;
	m_bModified = true;
	m_bInstanceStoreModified = true;
}

