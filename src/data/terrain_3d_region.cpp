// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/resource_saver.hpp>

#include <cstring>

#include "core/logger.h"
#include "data/terrain_3d_data.h"
#include "instances/terrain_3d_instance_page_codec.h"
#include "data/terrain_3d_region.h"
#include "core/terrain_3d_util.h"

namespace {

bool imageIsDefaultControl( const Ref<Image> &rMap, const int nRegionSize ) {
	if ( rMap.is_null() || rMap->get_format() != Image::FORMAT_RF || rMap->get_width() != nRegionSize ||
		rMap->get_height() != nRegionSize ) {
		return false;
	}
	const PackedByteArray data = rMap->get_data();
	const size_t texel_count = ( size_t ) ( nRegionSize ) * ( size_t ) ( nRegionSize );
	if ( ( size_t ) ( data.size() ) != texel_count * sizeof( uint32_t ) ) {
		return false;
	}
	const uint8_t *pBytes = data.ptr();
	for ( size_t index = 0; index < texel_count; ++index ) {
		uint32_t value = 0u;
		std::memcpy( &value, pBytes + index * sizeof( uint32_t ), sizeof( uint32_t ) );
		if ( value != TERRAIN3D_REGION_DEFAULT_CONTROL ) {
			return false;
		}
	}
	return true;
}

bool imageIsDefaultColor( const Ref<Image> &rMap, const int nRegionSize ) {
	if ( rMap.is_null() || rMap->get_format() != Image::FORMAT_RGBA8 || rMap->get_width() != nRegionSize ||
		rMap->get_height() != nRegionSize ) {
		return false;
	}
	const PackedByteArray data = rMap->get_data();
	const size_t texel_count = ( size_t ) ( nRegionSize ) * ( size_t ) ( nRegionSize );
	if ( ( size_t ) ( data.size() ) != texel_count * 4u ) {
		return false;
	}
	const uint32_t rgba = COLOR_ROUGHNESS.to_rgba32();
	const uint8_t expected[4] = {( uint8_t ) ( rgba >> 24u ), ( uint8_t ) ( rgba >> 16u ), ( uint8_t ) ( rgba >> 8u ), ( uint8_t ) ( rgba )};
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

int normalizeStreamedNodeBoundaryPolicy( const int nPolicy ) {
	return nPolicy == Terrain3DRegion::STREAMED_NODE_BOUNDARY_COVERAGE ?
		Terrain3DRegion::STREAMED_NODE_BOUNDARY_COVERAGE :
		Terrain3DRegion::STREAMED_NODE_BOUNDARY_ANCHOR;
}

void normalizeStreamedNodeCoverage(
	const int nPolicy, const Vector2i &rInputMin, const Vector2i &rInputMax,
	Vector2i &rOutputMin, Vector2i &rOutputMax
) {
	if ( nPolicy != Terrain3DRegion::STREAMED_NODE_BOUNDARY_COVERAGE ) {
		rOutputMin = V2I_ZERO;
		rOutputMax = V2I_ZERO;
		return;
	}
	const Vector2i inputMin = rInputMin;
	const Vector2i inputMax = rInputMax;
	rOutputMin = Vector2i( MIN( inputMin.x, inputMax.x ), MIN( inputMin.y, inputMax.y ) );
	rOutputMax = Vector2i( MAX( inputMin.x, inputMax.x ), MAX( inputMin.y, inputMax.y ) );
}

int normalizeLightingClass( const int nClass ) {
	return CLAMP( nClass, ( int ) ( Terrain3DRegion::LIGHTING_CLASS_TERRAIN ), ( int ) ( Terrain3DRegion::LIGHTING_CLASS_REGIONAL_OCCLUDER ) );
}

int normalizeBakedLightingMode( const int nMode ) {
	return CLAMP( nMode, ( int ) ( Terrain3DRegion::BAKED_LIGHTING_DISABLED ), ( int ) ( Terrain3DRegion::BAKED_LIGHTING_MATERIAL_ONLY ) );
}

void storeStreamedNodeLighting( Dictionary &rEntry, const Dictionary &dictSource ) {
	rEntry["lighting_class"] = normalizeLightingClass( dictSource.has( "lighting_class" ) ? ( int ) ( dictSource["lighting_class"] ) : ( int ) ( Terrain3DRegion::LIGHTING_CLASS_MEDIUM_OBSTACLE ) );
	rEntry["baked_lighting_mode"] = normalizeBakedLightingMode( dictSource.has( "baked_lighting_mode" ) ? ( int ) ( dictSource["baked_lighting_mode"] ) : ( int ) ( Terrain3DRegion::BAKED_LIGHTING_LIGHTMAP ) );
	rEntry["contribute_baked_gi"] = dictSource.has( "contribute_baked_gi" ) ? ( bool ) ( dictSource["contribute_baked_gi"] ) : true;
	rEntry["receive_dynamic_directional"] = dictSource.has( "receive_dynamic_directional" ) ? ( bool ) ( dictSource["receive_dynamic_directional"] ) : true;
	rEntry["dynamic_shadow_eligible"] = dictSource.has( "dynamic_shadow_eligible" ) ? ( bool ) ( dictSource["dynamic_shadow_eligible"] ) : true;
	if ( dictSource.has( "shadow_proxy_scene_path" ) && !String( dictSource["shadow_proxy_scene_path"] ).is_empty() ) {
		rEntry["shadow_proxy_scene_path"] = String( dictSource["shadow_proxy_scene_path"] );
	} else if ( dictSource.has( "shadow_proxy_scene" ) && Variant( dictSource["shadow_proxy_scene"] ).get_type() == Variant::OBJECT ) {
		rEntry["shadow_proxy_scene"] = dictSource["shadow_proxy_scene"];
	}
}

void storeStreamedNodeBoundary(
	Dictionary &rEntry, const int nPolicy,
	const Vector2i &rCoverageMin, const Vector2i &rCoverageMax
) {
	const int policy = normalizeStreamedNodeBoundaryPolicy( nPolicy );
	Vector2i coverage_min;
	Vector2i coverage_max;
	normalizeStreamedNodeCoverage( policy, rCoverageMin, rCoverageMax, coverage_min, coverage_max );
	rEntry["boundary_policy"] = policy;
	rEntry["coverage_min"] = coverage_min;
	rEntry["coverage_max"] = coverage_max;
}

} // namespace

/////////////////////
// Public Functions
/////////////////////

Terrain3DRegion::Terrain3DRegion( void ) {
}

Terrain3DRegion::~Terrain3DRegion( void ) {
}

void Terrain3DRegion::clear( void ) {
	m_fVersion = 0.8f;
	m_nRegionSize = 0;
	m_heightRange = V2_ZERO;
	m_pHeightMap.unref();
	m_pControlMap.unref();
	m_pColorMap.unref();
	m_pDerivedNormalSlopeMap.unref();
	m_bDerivedNormalSlopeValid = false;
	m_nChannelFlags = 0u;
	m_bRuntimeMapsModified = false;
	m_instanceStore.clear();
	m_instancePage.clear();
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = false;
	m_nCookedPageSourceSignature = 0u;
	clearInstanceMetadata();
	m_streamedNodes.clear();
	m_streamedNodeRuntime.clear();
	m_nNextStreamedNodeId = 1u;
	m_streamedNodeCoverageMin = V2I_ZERO;
	m_streamedNodeCoverageMax = V2I_ZERO;
	m_bStreamedNodeHasExtendedCoverage = false;
	m_pLighting.unref();
	m_fVertexSpacing = 1.f;
	m_bDeleted = false;
	m_bEdited = false;
	m_bModified = false;
	m_location = V2I_MAX;
}

void Terrain3DRegion::setVersion( const real_t fVersion ) {
	const real_t version = CLAMP( fVersion, 0.8f, 100.f );
	if ( m_fVersion == version ) {
		return;
	}
	// Mark modified if already initialized and we get a new value
	if ( m_fVersion > 0.8f ) {
		m_bModified = true;
	}
	m_fVersion = version;
	LOG( Terrain3D::DEBUG, vformat( "%.3f", m_fVersion ) );
	if ( m_fVersion < TERRAIN3D_DATA_CURRENT_VERSION ) {
		LOG( Terrain3D::WARN, "Region Data ", get_path(), " version ", vformat( "%.3f", m_fVersion ), " will be updated to ",
			vformat( "%.3f", TERRAIN3D_DATA_CURRENT_VERSION ), " upon save" );
	}
}

void Terrain3DRegion::setRegionSize( const int nRegionSize ) {
	if ( !isValidRegionSize( nRegionSize ) ) {
		LOG( Terrain3D::ERROR, "Invalid region size: ", nRegionSize, ". Must be power of 2, 64-2048" );
		return;
	}
	// Mark modified if already initialized and we get a new value
	if ( m_nRegionSize > 0 && m_nRegionSize != nRegionSize ) {
		m_bModified = true;
	}
	SET_IF_DIFF( m_nRegionSize, nRegionSize );
	t3dinstanceLayout_t default_layout;
	if ( m_instanceStore.empty() && terrain3dInstanceLayoutForRegion(
			nRegionSize, TERRAIN3D_INSTANCE_DEFAULT_CELL_SIZE, default_layout ) ) {
		m_instanceStore.setLayout( default_layout );
	}
	LOG( Terrain3D::DEBUG, "t3dsetting_t region ", m_location, " size: ", nRegionSize );
}

void Terrain3DRegion::setMap( const t3dmapType_t mapType, const Ref<Image> &rImage ) {
	switch ( mapType ) {
		case Terrain3DRegion::TYPE_HEIGHT:
			setHeightMap( rImage );
			break;
		case Terrain3DRegion::TYPE_CONTROL:
			setControlMap( rImage );
			break;
		case Terrain3DRegion::TYPE_COLOR:
			setColorMap( rImage );
			break;
		default:
			LOG( Terrain3D::ERROR, "Requested map type is invalid" );
			break;
	}
}

Ref<Image> Terrain3DRegion::getMap( const t3dmapType_t mapType ) const {
	switch ( mapType ) {
		case Terrain3DRegion::TYPE_HEIGHT:
			return getHeightMap();
		case Terrain3DRegion::TYPE_CONTROL:
			return getControlMap();
		case Terrain3DRegion::TYPE_COLOR:
			return getColorMap();
		default:
			LOG( Terrain3D::ERROR, "Requested map type ", mapType, ", is invalid" );
			return Ref<Image>();
	}
}

Image *Terrain3DRegion::getMapPtr( const t3dmapType_t mapType ) const {
	switch ( mapType ) {
		case Terrain3DRegion::TYPE_HEIGHT:
			return m_pHeightMap.is_valid() ? *m_pHeightMap : nullptr;
		case Terrain3DRegion::TYPE_CONTROL:
			return m_pControlMap.is_valid() ? *m_pControlMap : nullptr;
		case Terrain3DRegion::TYPE_COLOR:
			return m_pColorMap.is_valid() ? *m_pColorMap : nullptr;
		default:
			LOG( Terrain3D::ERROR, "Requested map type ", mapType, ", is invalid" );
			return nullptr;
	}
}

Ref<Image> Terrain3DRegion::materializeMap( const t3dmapType_t mapType ) {
	Ref<Image> pMap = getMap( mapType );
	if ( pMap.is_valid() ) {
		return pMap;
	}
	if ( !isValidRegionSize( m_nRegionSize ) ) {
		LOG( Terrain3D::ERROR, "Cannot materialize map before region_size is valid" );
		return Ref<Image>();
	}
	if ( mapType == Terrain3DRegion::TYPE_HEIGHT ) {
		pMap = Util::getFilledImage( V2I( m_nRegionSize ), Terrain3DRegionCodec::defaultHeightColor(), false,
							 Image::FORMAT_R16 );
	} else if ( mapType == Terrain3DRegion::TYPE_CONTROL ) {
		pMap = Util::getFilledImage( V2I( m_nRegionSize ), COLOR_CONTROL, false, Image::FORMAT_RF );
	} else if ( mapType == Terrain3DRegion::TYPE_COLOR ) {
		// CPU region color maps retain only their base level. GPU publication creates
		// a temporary mipmapped copy, avoiding 33% permanent CPU mip overhead.
		pMap = Util::getFilledImage( V2I( m_nRegionSize ), COLOR_ROUGHNESS, false, Image::FORMAT_RGBA8 );
	}
	if ( pMap.is_null() ) {
		return Ref<Image>();
	}

	// Writable materialization deliberately bypasses set_control_map()/setColorMap().
	// Those setters compact all-default images back to null, which is correct for
	// persistent assignment but would make an implicit channel impossible to edit.
	// Merely allocating the writable image is not itself an authored edit; callers
	// commit or compact it after they know whether any texel actually changed.
	if ( mapType == Terrain3DRegion::TYPE_HEIGHT ) {
		setHeightMap( pMap );
		return getHeightMap();
	} else if ( mapType == Terrain3DRegion::TYPE_CONTROL ) {
		m_pControlMap = pMap;
		m_nChannelFlags |= CHANNEL_CONTROL;
	} else if ( mapType == Terrain3DRegion::TYPE_COLOR ) {
		m_pColorMap = pMap;
		m_nChannelFlags |= CHANNEL_COLOR;
	}
	return pMap;
}

Image *Terrain3DRegion::getMapPtrForWrite( const t3dmapType_t mapType ) {
	Ref<Image> pMap = materializeMap( mapType );
	return pMap.is_valid() ? *pMap : nullptr;
}

void Terrain3DRegion::setMaps( const TypedArray<Image> &rMaps ) {
	if ( rMaps.size() != Terrain3DRegion::TYPE_MAX ) {
		LOG( Terrain3D::ERROR, "Expected ", Terrain3DRegion::TYPE_MAX - 1, " maps. Received ", rMaps.size() );
		return;
	}
	m_nRegionSize = 0;
	setHeightMap( rMaps[Terrain3DRegion::TYPE_HEIGHT] );
	setControlMap( rMaps[Terrain3DRegion::TYPE_CONTROL] );
	setColorMap( rMaps[Terrain3DRegion::TYPE_COLOR] );
}

TypedArray<Image> Terrain3DRegion::getMaps( void ) const {
	LOG( Terrain3D::DEBUG, "Retrieving maps from region: ", m_location );
	TypedArray<Image> maps;
	maps.push_back( m_pHeightMap );
	maps.push_back( m_pControlMap );
	maps.push_back( m_pColorMap );
	return maps;
}

void Terrain3DRegion::setHeightMap( const Ref<Image> &rMap ) {
	LOG( Terrain3D::DEBUG, "t3dsetting_t height map for region: ", ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)" );
	if ( m_nRegionSize == 0 && rMap.is_valid() ) {
		setRegionSize( rMap->get_width() );
	}
	const Ref<Image> pOldMap = m_pHeightMap;
	const Ref<Image> pMap = sanitizeMap( Terrain3DRegion::TYPE_HEIGHT, rMap );
	if ( rMap.is_valid() && pMap.is_null() ) {
		LOG( Terrain3D::ERROR, "Height map assignment was rejected; existing region storage remains unchanged" );
		return;
	}
	if ( pOldMap == pMap ) {
		return;
	}
	m_pHeightMap = pMap;
	m_bDerivedNormalSlopeValid = false;
	m_bRuntimeMapsModified = true;
	m_bModified = true;
	calcHeightRange();
}

void Terrain3DRegion::setControlMap( const Ref<Image> &rMap ) {
	LOG( Terrain3D::DEBUG, "t3dsetting_t control map for region: ", ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)" );
	if ( m_nRegionSize == 0 && rMap.is_valid() ) {
		setRegionSize( rMap->get_width() );
	}
	const Ref<Image> pOldMap = m_pControlMap;
	Ref<Image> pMap = sanitizeMap( Terrain3DRegion::TYPE_CONTROL, rMap );
	if ( imageIsDefaultControl( pMap, m_nRegionSize ) ) {
		pMap.unref();
	}
	if ( pOldMap == pMap ) {
		return;
	}
	m_pControlMap = pMap;
	if ( m_pControlMap.is_valid() ) {
		m_nChannelFlags |= CHANNEL_CONTROL;
	} else {
		m_nChannelFlags &= ( uint8_t ) ( ~CHANNEL_CONTROL );
	}
	m_bRuntimeMapsModified = true;
	m_bModified = true;
}

void Terrain3DRegion::setColorMap( const Ref<Image> &rMap ) {
	LOG( Terrain3D::DEBUG, "t3dsetting_t color map for region: ", ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)" );
	if ( m_nRegionSize == 0 && rMap.is_valid() ) {
		setRegionSize( rMap->get_width() );
	}
	const Ref<Image> pOldMap = m_pColorMap;
	Ref<Image> pMap = sanitizeMap( Terrain3DRegion::TYPE_COLOR, rMap );
	if ( imageIsDefaultColor( pMap, m_nRegionSize ) ) {
		pMap.unref();
	}
	if ( pOldMap == pMap ) {
		return;
	}
	m_pColorMap = pMap;
	if ( m_pColorMap.is_valid() ) {
		m_nChannelFlags |= CHANNEL_COLOR;
	} else {
		m_nChannelFlags &= ( uint8_t ) ( ~CHANNEL_COLOR );
	}
	m_bRuntimeMapsModified = true;
	m_bModified = true;
}

void Terrain3DRegion::setChannelFlags( const int nFlags ) {
	uint8_t flags = ( uint8_t ) ( nFlags ) & CHANNEL_MASK;
	// Image payloads are authoritative when present. This makes property load
	// order irrelevant for old and new .res files.
	if ( m_pControlMap.is_valid() ) {
		flags |= CHANNEL_CONTROL;
	}
	if ( m_pColorMap.is_valid() ) {
		flags |= CHANNEL_COLOR;
	}
	if ( m_pDerivedNormalSlopeMap.is_valid() ) {
		flags |= CHANNEL_DERIVED;
	}
	m_nChannelFlags = flags;
}

void Terrain3DRegion::setDerivedNormalSlopeMapInternal( const Ref<Image> &rMap ) {
	if ( rMap.is_null() || rMap->get_width() != m_nRegionSize || rMap->get_height() != m_nRegionSize ||
		rMap->get_format() != Image::FORMAT_RG8 ) {
		m_pDerivedNormalSlopeMap.unref();
		m_bDerivedNormalSlopeValid = false;
		return;
	}
	m_pDerivedNormalSlopeMap = rMap;
	m_bDerivedNormalSlopeValid = true;
	m_nChannelFlags |= CHANNEL_DERIVED;
}

void Terrain3DRegion::sanitizeMaps( void ) {
	if ( m_nRegionSize == 0 ) { // blank region, no set_*_map has been called
		LOG( Terrain3D::ERROR, "Set region_size first" );
		return;
	}
	Ref<Image> pMap = sanitizeMap( Terrain3DRegion::TYPE_HEIGHT, m_pHeightMap );
	if ( m_pHeightMap != pMap ) {
		m_bRuntimeMapsModified = true;
		m_bModified = true;
	}
	m_pHeightMap = pMap;
	m_bDerivedNormalSlopeValid = false;
	if ( m_pControlMap.is_valid() ) {
		pMap = sanitizeMap( Terrain3DRegion::TYPE_CONTROL, m_pControlMap );
		if ( m_pControlMap != pMap ) {
			m_bRuntimeMapsModified = true;
			m_bModified = true;
		}
		m_pControlMap = pMap;
	}
	if ( m_pColorMap.is_valid() ) {
		pMap = sanitizeMap( Terrain3DRegion::TYPE_COLOR, m_pColorMap );
		if ( m_pColorMap != pMap ) {
			m_bRuntimeMapsModified = true;
			m_bModified = true;
		}
		m_pColorMap = pMap;
	}
	setChannelFlags( m_nChannelFlags );
	compactDefaultMaps();
}

bool Terrain3DRegion::isMapDefault( const t3dmapType_t mapType ) const {
	switch ( mapType ) {
		case Terrain3DRegion::TYPE_CONTROL:
			return m_pControlMap.is_null() || imageIsDefaultControl( m_pControlMap, m_nRegionSize );
		case Terrain3DRegion::TYPE_COLOR:
			return m_pColorMap.is_null() || imageIsDefaultColor( m_pColorMap, m_nRegionSize );
		default:
			return false;
	}
}

bool Terrain3DRegion::compactMapIfDefault( const t3dmapType_t mapType, const bool bMarkModified ) {
	Ref<Image> *pMap = nullptr;
	switch ( mapType ) {
		case Terrain3DRegion::TYPE_CONTROL:
			pMap = &m_pControlMap;
			break;
		case Terrain3DRegion::TYPE_COLOR:
			pMap = &m_pColorMap;
			break;
		default:
			return false;
	}
	if ( pMap->is_null() || !isMapDefault( mapType ) ) {
		return false;
	}
	pMap->unref();
	if ( mapType == Terrain3DRegion::TYPE_CONTROL ) {
		m_nChannelFlags &= ( uint8_t ) ( ~CHANNEL_CONTROL );
	} else if ( mapType == Terrain3DRegion::TYPE_COLOR ) {
		m_nChannelFlags &= ( uint8_t ) ( ~CHANNEL_COLOR );
	}
	if ( bMarkModified ) {
		m_bRuntimeMapsModified = true;
		m_bModified = true;
	}
	return true;
}

void Terrain3DRegion::compactDefaultMaps( void ) {
	compactMapIfDefault( Terrain3DRegion::TYPE_CONTROL );
	compactMapIfDefault( Terrain3DRegion::TYPE_COLOR );
}

void Terrain3DRegion::markMapEdited( const t3dmapType_t mapType, const bool bRecalculateHeightRange ) {
	if ( mapType < Terrain3DRegion::TYPE_HEIGHT || mapType >= Terrain3DRegion::TYPE_MAX ) {
		LOG( Terrain3D::ERROR, "Invalid map type: ", mapType );
		return;
	}
	m_bRuntimeMapsModified = true;
	m_bModified = true;
	m_bEdited = true;
	if ( mapType == Terrain3DRegion::TYPE_HEIGHT ) {
		m_bDerivedNormalSlopeValid = false;
	}
	if ( mapType == Terrain3DRegion::TYPE_HEIGHT && bRecalculateHeightRange ) {
		calcHeightRange();
	}
}

Ref<Image> Terrain3DRegion::sanitizeMap( const t3dmapType_t mapType, const Ref<Image> &rMap ) const {
	LOG( Terrain3D::DEBUG, "Sanitizing map type: ", mapType, ", map: ", rMap );
	if ( !isValidRegionSize( m_nRegionSize ) ) {
		LOG( Terrain3D::ERROR, "Invalid region size: ", m_nRegionSize, ". Set it or set a map first. Must be power of 2, 64-2048" );
		return Ref<Image>();
	}
	if ( mapType != Terrain3DRegion::TYPE_HEIGHT && rMap.is_null() ) {
		// Missing control/color maps are implicit defaults until first write.
		return Ref<Image>();
	}
	if ( mapType == Terrain3DRegion::TYPE_HEIGHT ) {
		if ( rMap.is_valid() && rMap->get_format() == Image::FORMAT_R16 &&
			!Terrain3DRegionCodec::hasValidHeightPayload( rMap, m_nRegionSize ) ) {
			LOG( Terrain3D::ERROR, "Rejected malformed R16 height payload for region ", m_location, ": expected ",
				Terrain3DRegionCodec::expectedHeightByteCount( m_nRegionSize, Image::FORMAT_R16 ), " bytes, received ",
				rMap->get_data().size() );
			return Ref<Image>();
		}

		uint32_t clamped_count = 0u;
		uint32_t non_finite_count = 0u;
		Ref<Image> pConverted =
			Terrain3DRegionCodec::convertHeightToR16( rMap, m_nRegionSize, clamped_count, non_finite_count );
		if ( rMap.is_valid() && rMap->get_format() == Image::FORMAT_RF && pConverted.is_valid() ) {
			LOG( Terrain3D::INFO, "Converted internal RF32 height storage to R16 for region ", m_location, " (",
				m_nRegionSize * m_nRegionSize, " samples)" );
		}
		if ( clamped_count > 0u || non_finite_count > 0u ) {
			LOG( Terrain3D::WARN, "R16 height conversion for region ", m_location, " clamped ", clamped_count,
				" samples and replaced ", non_finite_count, " non-finite samples" );
		}
		return pConverted;
	}
	const char *type_str = TERRAIN3D_REGION_MAP_TYPE_NAMES[mapType];
	const Image::Format format = TERRAIN3D_REGION_MAP_FORMATS[mapType];
	Color color = TERRAIN3D_REGION_MAP_DEFAULT_COLORS[mapType];
	Ref<Image> pMap;

	if ( rMap.is_valid() ) {
		if ( validateMapSize( rMap ) ) {
			if ( rMap->get_format() == format ) {
				LOG( Terrain3D::DEBUG, "Map type ", type_str, " correct format, size. Mipmaps: ", rMap->has_mipmaps() );
				pMap = rMap;
			} else {
				LOG( Terrain3D::DEBUG, "Provided ", type_str, " map wrong format: ", rMap->get_format(),
					". Converting copy to: ", format );
				pMap.instantiate();
				pMap->copy_from( rMap );
				pMap->convert( format );
				if ( pMap->get_format() != format ) {
					LOG( Terrain3D::DEBUG, "Cannot convert image to format: ", format, ". Creating blank " );
					pMap.unref();
				}
			}
		} else {
			LOG( Terrain3D::DEBUG, "Provided ", type_str, " map wrong size: ", rMap->get_size(), ". Creating blank" );
		}
	} else {
		LOG( Terrain3D::DEBUG, "No provided ", type_str, " map. Creating blank" );
	}
	if ( pMap.is_null() ) {
		LOG( Terrain3D::DEBUG, "Making new image of type: ", type_str );
		return Util::getFilledImage( V2I( m_nRegionSize ), color, false, format );
	}
	if ( mapType == Terrain3DRegion::TYPE_COLOR && pMap->has_mipmaps() ) {
		Ref<Image> pBaseOnly;
		pBaseOnly.instantiate();
		pBaseOnly->copy_from( pMap );
		pBaseOnly->clear_mipmaps();
		return pBaseOnly;
	}
	return pMap;
}

bool Terrain3DRegion::validateMapSize( const Ref<Image> &rMap ) const {
	const Vector2i region_sizev = rMap->get_size();
	if ( region_sizev.x != region_sizev.y ) {
		LOG( Terrain3D::ERROR, "Image width doesn't match height: ", region_sizev );
		return false;
	}
	if ( !isValidRegionSize( region_sizev.x ) || !isValidRegionSize( region_sizev.y ) ) {
		LOG( Terrain3D::ERROR, "Invalid image size: ", region_sizev, ". Must be power of 2, 64-2048 and square" );
		return false;
	}
	if ( m_nRegionSize != region_sizev.x || m_nRegionSize != region_sizev.y ) {
		LOG( Terrain3D::ERROR, "Image size doesn't match existing images in this region", region_sizev );
		return false;
	}
	return true;
}

void Terrain3DRegion::setHeightRange( const Vector2 &rRange ) {
	if ( differs( m_heightRange, rRange ) ) {
		// Mark modified if setting after initialization
		if ( !m_heightRange.is_zero_approx() ) {
			m_bModified = true;
		}
		m_heightRange = rRange;
		LOG( Terrain3D::DEBUG, vformat( "%.2v", rRange ) );
	} else {
		return;
	};
}

void Terrain3DRegion::updateHeight( const real_t fHeight ) {
	if ( fHeight < m_heightRange.x ) {
		m_heightRange.x = fHeight;
		m_bModified = true;
	} else if ( fHeight > m_heightRange.y ) {
		m_heightRange.y = fHeight;
		m_bModified = true;
	}
}

void Terrain3DRegion::updateHeights( const Vector2 &rLowHigh ) {
	if ( rLowHigh.x < m_heightRange.x ) {
		m_heightRange.x = rLowHigh.x;
		m_bModified = true;
	}
	if ( rLowHigh.y > m_heightRange.y ) {
		m_heightRange.y = rLowHigh.y;
		m_bModified = true;
	}
}

void Terrain3DRegion::calcHeightRange( void ) {
	const Vector2 range = Terrain3DRegionCodec::getHeightMinMax( m_pHeightMap );
	if ( m_heightRange != range ) {
		m_heightRange = range;
		m_bModified = true;
		LOG( Terrain3D::DEBUG, "Recalculated new height range: ", m_heightRange,
			" for region: ", ( m_location.x != INT32_MAX ) ? String( m_location ) : "(new)", ". Marking modified" );
	}
}

void Terrain3DRegion::adoptCookedRuntimeMapsInternal(
	const Ref<Image> &rHeight, const Ref<Image> &rControl, const Ref<Image> &rColor, const Ref<Image> &rDerived
) {
	if ( rHeight.is_null() || rHeight->get_width() != m_nRegionSize || rHeight->get_height() != m_nRegionSize ) {
		return;
	}
	m_pHeightMap = rHeight;
	m_pControlMap = rControl;
	m_pColorMap = rColor;
	m_pDerivedNormalSlopeMap = rDerived;
	m_bDerivedNormalSlopeValid = rDerived.is_valid();
	m_nChannelFlags |= CHANNEL_HEIGHT;
	if ( rControl.is_valid() ) m_nChannelFlags |= CHANNEL_CONTROL; else m_nChannelFlags &= ~CHANNEL_CONTROL;
	if ( rColor.is_valid() ) m_nChannelFlags |= CHANNEL_COLOR; else m_nChannelFlags &= ~CHANNEL_COLOR;
	if ( rDerived.is_valid() ) m_nChannelFlags |= CHANNEL_DERIVED; else m_nChannelFlags &= ~CHANNEL_DERIVED;
	m_bRuntimeMapsModified = false;
}

void Terrain3DRegion::importRuntimeMapsUnmodified( const Terrain3DRegion *pSource ) {
	if ( !pSource || pSource->m_nRegionSize != m_nRegionSize ) {
		return;
	}
	m_pHeightMap = pSource->m_pHeightMap;
	m_pControlMap = pSource->m_pControlMap;
	m_pColorMap = pSource->m_pColorMap;
	m_pDerivedNormalSlopeMap = pSource->m_pDerivedNormalSlopeMap;
	m_bDerivedNormalSlopeValid = pSource->m_bDerivedNormalSlopeValid;
	m_nChannelFlags = pSource->m_nChannelFlags;
	m_bRuntimeMapsModified = pSource->m_bRuntimeMapsModified;
	if ( m_bRuntimeMapsModified ) {
		m_bModified = true;
	}
}

void Terrain3DRegion::releaseRuntimeMapsUnmodified( void ) {
	m_pHeightMap.unref();
	m_pControlMap.unref();
	m_pColorMap.unref();
	m_pDerivedNormalSlopeMap.unref();
	m_bDerivedNormalSlopeValid = false;
}

void Terrain3DRegion::markRuntimeMapsClean( void ) {
	m_bRuntimeMapsModified = false;
}

bool Terrain3DRegion::hasRuntimeMapPayload( void ) const {
	return m_pHeightMap.is_valid();
}

bool Terrain3DRegion::runtimeMapsEvictable( void ) const {
	return !m_bRuntimeMapsModified && !m_bEdited && !m_bDeleted && !get_path().is_empty();
}

void Terrain3DRegion::clearInstanceMetadata( void ) const {
	std::memset( m_szInstanceMeshCounts, 0, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = 0u;
	m_nInstanceMeshCount = 0u;
}

bool Terrain3DRegion::encodeInstancePage( void ) const {
	if ( !m_bInstanceStoreResident ) {
		return true;
	}
	if ( !m_bInstanceStoreModified && !m_instancePage.is_empty() ) {
		return true;
	}
	PackedByteArray page;
	uint32_t instance_count = 0u;
	uint16_t mesh_count = 0u;
	uint32_t mesh_counts[TERRAIN3D_MAX_MESH_ASSETS] = {};
	if ( !T3DInstancePageCodec::encode( m_instanceStore, page, mesh_counts, instance_count, mesh_count ) ) {
		LOG( Terrain3D::ERROR, "Failed encoding instance page for region ", m_location );
		return false;
	}
	m_instancePage = page;
	std::memcpy( m_szInstanceMeshCounts, mesh_counts, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = instance_count;
	m_nInstanceMeshCount = mesh_count;
	m_bInstanceStoreModified = false;
	return true;
}

void Terrain3DRegion::importInstancesUnmodified( const Dictionary &rInstances ) {
	m_instanceStore.importDictionary( rInstances );
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = true;
	m_instancePage.clear();
	m_cookedInstanceMetadata.clear();
	m_cookedMultimeshFastStart.clear();
	if ( !encodeInstancePage() ) {
		clearInstanceMetadata();
	}
	LOG( Terrain3D::DEBUG, "Region ", m_location, " imported ", ( int ) ( m_nInstanceCount ), " native instances" );
}

void Terrain3DRegion::setInstances( const Dictionary &rInstances ) {
	ensureInstanceStoreResident();
	if ( m_instanceStore.equalsDictionary( rInstances ) ) {
		return;
	}
	importInstancesUnmodified( rInstances );
	m_bInstanceStoreModified = true;
	m_bModified = true;
}

Dictionary Terrain3DRegion::getInstancesLegacyStorage( void ) const {
	// New saves use instance_page. Keep only the setter-backed legacy property so
	// old .res files still load without writing a duplicate Dictionary payload.
	return Dictionary();
}

Dictionary Terrain3DRegion::getInstances( void ) const {
	if ( !ensureInstanceStoreResident() ) {
		return Dictionary();
	}
	return m_instanceStore.exportDictionary();
}

void Terrain3DRegion::setInstancePage( const PackedByteArray &rPage ) {
	if ( rPage == m_instancePage && !m_bInstanceStoreModified ) {
		return;
	}
	uint32_t mesh_counts[TERRAIN3D_MAX_MESH_ASSETS] = {};
	uint32_t instance_count = 0u;
	uint16_t mesh_count = 0u;
	if ( !T3DInstancePageCodec::inspect( rPage, mesh_counts, instance_count, mesh_count ) ) {
		LOG( Terrain3D::ERROR, "Rejected invalid instance page for region ", m_location );
		return;
	}
	m_instancePage = rPage;
	m_instanceStore.clear();
	m_bInstanceStoreResident = rPage.is_empty();
	m_bInstanceStoreModified = false;
	std::memcpy( m_szInstanceMeshCounts, mesh_counts, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = instance_count;
	m_nInstanceMeshCount = mesh_count;
}

PackedByteArray Terrain3DRegion::getInstancePage( void ) const {
	if ( !encodeInstancePage() ) {
		return PackedByteArray();
	}
	return m_instancePage;
}

bool Terrain3DRegion::ensureInstanceStoreResident( void ) const {
	if ( m_bInstanceStoreResident ) {
		return true;
	}
	T3DInstanceStore decoded;
	uint32_t mesh_counts[TERRAIN3D_MAX_MESH_ASSETS] = {};
	uint32_t instance_count = 0u;
	uint16_t mesh_count = 0u;
	if ( !T3DInstancePageCodec::decode( m_instancePage, decoded, mesh_counts, instance_count, mesh_count ) ) {
		LOG( Terrain3D::ERROR, "Failed decoding instance page for region ", m_location );
		return false;
	}
	m_instanceStore = eastl::move( decoded );
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = false;
	std::memcpy( m_szInstanceMeshCounts, mesh_counts, sizeof( m_szInstanceMeshCounts ) );
	m_nInstanceCount = instance_count;
	m_nInstanceMeshCount = mesh_count;
	return true;
}

void Terrain3DRegion::adoptPreparedInstanceStoreInternal( T3DInstanceStore &&rStore ) {
	m_instanceStore = eastl::move( rStore );
	m_bInstanceStoreResident = true;
	m_bInstanceStoreModified = false;
}

void Terrain3DRegion::adoptCookedInstanceAccelerationInternal(
	const PackedByteArray &rMetadata, const PackedByteArray &rFastStart
) {
	m_cookedInstanceMetadata = rMetadata;
	m_cookedMultimeshFastStart = rFastStart;
}

void Terrain3DRegion::setCookedInstanceMetadataInternal(
	const uint32_t *const pMeshCounts, const uint32_t nInstanceCount, const uint16_t nMeshCount
) {
	if ( pMeshCounts ) {
		std::memcpy( m_szInstanceMeshCounts, pMeshCounts, sizeof( m_szInstanceMeshCounts ) );
	} else {
		std::memset( m_szInstanceMeshCounts, 0, sizeof( m_szInstanceMeshCounts ) );
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
	const uint16_t nCount, const Vector2i &rCoverageMin, const Vector2i &rCoverageMax
) {
	m_nCookedStreamedNodeCount = nCount;
	m_cookedStreamedNodeCoverageMin = rCoverageMin;
	m_cookedStreamedNodeCoverageMax = rCoverageMax;
}

void Terrain3DRegion::adoptCookedStreamedNodeRuntimeInternal( eastl::vector<t3dstreamedNodeRuntimeDescriptor_t> &&rNodes ) {
	m_streamedNodeRuntime = eastl::move( rNodes );
	m_nCookedStreamedNodeCount = ( uint16_t ) ( MIN( m_streamedNodeRuntime.size(), ( size_t ) ( UINT16_MAX ) ) );
	refreshStreamedNodeCoverage();
	m_bModified = false;
}

bool Terrain3DRegion::evictInstanceStore( void ) {
	if ( !m_bInstanceStoreResident ) {
		return true;
	}
	if ( !encodeInstancePage() ) {
		return false;
	}
	T3DInstanceStore empty;
	m_instanceStore = eastl::move( empty );
	m_cookedInstanceMetadata.clear();
	m_cookedMultimeshFastStart.clear();
	m_bInstanceStoreResident = false;
	return true;
}

bool Terrain3DRegion::streamedNodeIdExists( const uint64_t nId ) const {
	if ( nId == 0u || nId > ( uint64_t ) ( INT64_MAX ) ) {
		return false;
	}
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		const Dictionary entry = m_streamedNodes[index];
		if ( entry.has( "id" ) && ( int64_t ) ( entry["id"] ) > 0 && ( uint64_t ) ( ( int64_t ) ( entry["id"] ) ) == nId ) {
			return true;
		}
	}
	return false;
}

int64_t Terrain3DRegion::allocateStreamedNodeId( const int64_t nRequestedId ) {
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

void Terrain3DRegion::refreshStreamedNodeId( void ) {
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

void Terrain3DRegion::rebuildStreamedNodeRuntime( void ) {
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
		t3dstreamedNodeRuntimeDescriptor_t descriptor;
		descriptor.id = ( int64_t ) ( entry["id"] );
		descriptor.pScene = entry.has( "scene" ) ? Ref<PackedScene>( entry["scene"] ) : Ref<PackedScene>();
		descriptor.scenePath = entry.has( "scene_path" ) ? String( entry["scene_path"] ) : String();
		descriptor.transform = Transform3D( entry["transform"] );
		descriptor.state = entry.has( "state" ) && Variant( entry["state"] ).get_type() == Variant::DICTIONARY ?
			Dictionary( entry["state"] ) : Dictionary();
		descriptor.flags = entry.has( "flags" ) ? ( int ) ( entry["flags"] ) : 0;
		descriptor.hlodMinLevel = CLAMP( entry.has( "hlod_min_level" ) ? ( int ) entry["hlod_min_level"] : 1, 1, 6 );
		descriptor.boundaryPolicy = entry.has( "boundary_policy" ) ?
			normalizeStreamedNodeBoundaryPolicy( ( int ) ( entry["boundary_policy"] ) ) :
			STREAMED_NODE_BOUNDARY_ANCHOR;
		descriptor.coverageMin = entry.has( "coverage_min" ) ? Vector2i( entry["coverage_min"] ) : V2I_ZERO;
		descriptor.coverageMax = entry.has( "coverage_max" ) ? Vector2i( entry["coverage_max"] ) : V2I_ZERO;
		normalizeStreamedNodeCoverage( descriptor.boundaryPolicy, descriptor.coverageMin,
			descriptor.coverageMax, descriptor.coverageMin, descriptor.coverageMax );
		descriptor.lightingClass = normalizeLightingClass( entry.has( "lighting_class" ) ? ( int ) ( entry["lighting_class"] ) : ( int ) ( LIGHTING_CLASS_MEDIUM_OBSTACLE ) );
		descriptor.bakedLightingMode = normalizeBakedLightingMode( entry.has( "baked_lighting_mode" ) ? ( int ) ( entry["baked_lighting_mode"] ) : ( int ) ( BAKED_LIGHTING_LIGHTMAP ) );
		descriptor.bContributeBakedGi = entry.has( "contribute_baked_gi" ) ? ( bool ) ( entry["contribute_baked_gi"] ) : true;
		descriptor.bReceiveDynamicDirectional = entry.has( "receive_dynamic_directional" ) ? ( bool ) ( entry["receive_dynamic_directional"] ) : true;
		descriptor.bDynamicShadowEligible = entry.has( "dynamic_shadow_eligible" ) ? ( bool ) ( entry["dynamic_shadow_eligible"] ) : true;
		descriptor.shadowProxyScenePath = entry.has( "shadow_proxy_scene_path" ) ? String( entry["shadow_proxy_scene_path"] ) : String();
		descriptor.pShadowProxyScene = entry.has( "shadow_proxy_scene" ) ? Ref<PackedScene>( entry["shadow_proxy_scene"] ) : Ref<PackedScene>();
		m_streamedNodeRuntime.push_back( eastl::move( descriptor ) );
	}
}

void Terrain3DRegion::refreshStreamedNodeCoverage( void ) {
	m_streamedNodeCoverageMin = V2I_ZERO;
	m_streamedNodeCoverageMax = V2I_ZERO;
	m_bStreamedNodeHasExtendedCoverage = false;
	for ( const t3dstreamedNodeRuntimeDescriptor_t &descriptor : m_streamedNodeRuntime ) {
		if ( descriptor.boundaryPolicy != STREAMED_NODE_BOUNDARY_COVERAGE ) {
			continue;
		}
		const Vector2i coverage_min = descriptor.coverageMin;
		const Vector2i coverage_max = descriptor.coverageMax;
		m_streamedNodeCoverageMin.x = MIN( m_streamedNodeCoverageMin.x, coverage_min.x );
		m_streamedNodeCoverageMin.y = MIN( m_streamedNodeCoverageMin.y, coverage_min.y );
		m_streamedNodeCoverageMax.x = MAX( m_streamedNodeCoverageMax.x, coverage_max.x );
		m_streamedNodeCoverageMax.y = MAX( m_streamedNodeCoverageMax.y, coverage_max.y );
		if ( coverage_min != V2I_ZERO || coverage_max != V2I_ZERO ) {
			m_bStreamedNodeHasExtendedCoverage = true;
		}
	}
}

bool Terrain3DRegion::streamedNodeCoverageIntersects(
	const Vector2i &rOwnerLocation,
	const Vector2i &rTargetMin, const Vector2i &vTargetMax
) const {
	if ( m_streamedNodeRuntime.empty() ) {
		if ( m_nCookedStreamedNodeCount == 0u ) {
			return false;
		}
		const Vector2i cooked_min = rOwnerLocation + m_cookedStreamedNodeCoverageMin;
		const Vector2i cooked_max = rOwnerLocation + m_cookedStreamedNodeCoverageMax;
		return rOwnerLocation.x >= rTargetMin.x && rOwnerLocation.x <= vTargetMax.x &&
			rOwnerLocation.y >= rTargetMin.y && rOwnerLocation.y <= vTargetMax.y ||
			( cooked_min.x <= vTargetMax.x && cooked_max.x >= rTargetMin.x &&
				cooked_min.y <= vTargetMax.y && cooked_max.y >= rTargetMin.y );
	}
	if ( rOwnerLocation.x >= rTargetMin.x && rOwnerLocation.x <= vTargetMax.x &&
		rOwnerLocation.y >= rTargetMin.y && rOwnerLocation.y <= vTargetMax.y ) {
		return true;
	}
	if ( !m_bStreamedNodeHasExtendedCoverage ) {
		return false;
	}
	const Vector2i aggregate_min = rOwnerLocation + m_streamedNodeCoverageMin;
	const Vector2i aggregate_max = rOwnerLocation + m_streamedNodeCoverageMax;
	if ( aggregate_min.x > vTargetMax.x || aggregate_max.x < rTargetMin.x ||
		aggregate_min.y > vTargetMax.y || aggregate_max.y < rTargetMin.y ) {
		return false;
	}
	for ( const t3dstreamedNodeRuntimeDescriptor_t &descriptor : m_streamedNodeRuntime ) {
		if ( descriptor.boundaryPolicy != STREAMED_NODE_BOUNDARY_COVERAGE ) {
			continue;
		}
		const Vector2i coverage_min = rOwnerLocation + descriptor.coverageMin;
		const Vector2i coverage_max = rOwnerLocation + descriptor.coverageMax;
		if ( coverage_min.x <= vTargetMax.x && coverage_max.x >= rTargetMin.x &&
			coverage_min.y <= vTargetMax.y && coverage_max.y >= rTargetMin.y ) {
			return true;
		}
	}
	return false;
}

const Terrain3DRegion::t3dstreamedNodeRuntimeDescriptor_t *Terrain3DRegion::findStreamedNodeRuntimeInternal(
	const int64_t nId
) const {
	for ( const t3dstreamedNodeRuntimeDescriptor_t &descriptor : m_streamedNodeRuntime ) {
		if ( descriptor.id == nId ) {
			return &descriptor;
		}
	}
	return nullptr;
}

void Terrain3DRegion::setStreamedNodes( const Array &rNodes ) {
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
		const String scene_path = source.has( "scene_path" ) ? String( source["scene_path"] ) : String();
		const Ref<PackedScene> pScene = source.has( "scene" ) ? Ref<PackedScene>( source["scene"] ) : Ref<PackedScene>();
		if ( scene_path.is_empty() && pScene.is_null() ) {
			continue;
		}
		const int64_t requested_id = source.has( "id" ) ? ( int64_t ) ( source["id"] ) : 0;
		const int64_t id = allocateStreamedNodeId( requested_id );
		if ( id <= 0 ) {
			LOG( Terrain3D::WARN, "Skipped duplicate streamed-node id ", requested_id, " in region ", m_location );
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
		entry["state"] = source.has( "state" ) && Variant( source["state"] ).get_type() == Variant::DICTIONARY ?
			Dictionary( source["state"] ).duplicate( true ) : Dictionary();
		entry["flags"] = source.has( "flags" ) ? ( int ) ( source["flags"] ) : 0;
		entry["hlod_min_level"] = CLAMP( source.has( "hlod_min_level" ) ? ( int ) source["hlod_min_level"] : 1, 1, 6 );
		storeStreamedNodeBoundary( entry,
			source.has( "boundary_policy" ) ? ( int ) ( source["boundary_policy"] ) : STREAMED_NODE_BOUNDARY_ANCHOR,
			source.has( "coverage_min" ) ? Vector2i( source["coverage_min"] ) : V2I_ZERO,
			source.has( "coverage_max" ) ? Vector2i( source["coverage_max"] ) : V2I_ZERO );
		storeStreamedNodeLighting( entry, source );
		m_streamedNodes.push_back( entry );
	}
	refreshStreamedNodeId();
	rebuildStreamedNodeRuntime();
	refreshStreamedNodeCoverage();
	m_bModified = true;
}

int64_t Terrain3DRegion::addStreamedNode(
	const Ref<PackedScene> &rScene, const Transform3D &rLocalTransform,
	const Dictionary &rState, const int nFlags, const int64_t nId, const int nBoundaryPolicy,
	const Vector2i &rCoverageMin, const Vector2i &rCoverageMax, const int nHlodMinLevel
) {
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
	storeStreamedNodeBoundary( entry, nBoundaryPolicy, rCoverageMin, rCoverageMax );
	storeStreamedNodeLighting( entry, Dictionary() );
	m_streamedNodes.push_back( entry );
	rebuildStreamedNodeRuntime();
	refreshStreamedNodeCoverage();
	m_bModified = true;
	return id;
}

int64_t Terrain3DRegion::addStreamedNodePath(
	const String &rScenePath,
	const Transform3D &rLocalTransform, const Dictionary &rState, const int nFlags, const int64_t nId,
	const int nBoundaryPolicy, const Vector2i &rCoverageMin, const Vector2i &rCoverageMax, const int nHlodMinLevel
) {
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
	storeStreamedNodeBoundary( entry, nBoundaryPolicy, rCoverageMin, rCoverageMax );
	storeStreamedNodeLighting( entry, Dictionary() );
	m_streamedNodes.push_back( entry );
	rebuildStreamedNodeRuntime();
	refreshStreamedNodeCoverage();
	m_bModified = true;
	return id;
}

bool Terrain3DRegion::removeStreamedNode( const int64_t nId ) {
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		const Dictionary entry = m_streamedNodes[index];
		if ( entry.has( "id" ) && ( int64_t ) ( entry["id"] ) == nId ) {
			m_streamedNodes.remove_at( index );
			rebuildStreamedNodeRuntime();
			refreshStreamedNodeCoverage();
			m_bModified = true;
			return true;
		}
	}
	return false;
}

bool Terrain3DRegion::updateStreamedNodeState(
	const int64_t nId, const Transform3D &rLocalTransform,
	const Dictionary &rState, const int nFlags
) {
	for ( int index = 0; index < m_streamedNodes.size(); ++index ) {
		Dictionary entry = m_streamedNodes[index];
		if ( !entry.has( "id" ) || ( int64_t ) ( entry["id"] ) != nId ) {
			continue;
		}
		const Transform3D old_transform = entry.has( "transform" ) ? Transform3D( entry["transform"] ) : Transform3D();
		const Dictionary old_state = entry.has( "state" ) ? Dictionary( entry["state"] ) : Dictionary();
		const int old_flags = entry.has( "flags" ) ? ( int ) ( entry["flags"] ) : 0;
		if ( old_transform.is_equal_approx( rLocalTransform ) && old_state == rState && old_flags == nFlags ) {
			return true;
		}
		entry["transform"] = rLocalTransform;
		entry["state"] = rState.duplicate( true );
		entry["flags"] = nFlags;
		m_streamedNodes[index] = entry;
		rebuildStreamedNodeRuntime();
		m_bModified = true;
		return true;
	}
	return false;
}

void Terrain3DRegion::setLighting( const Ref<Terrain3DRegionLighting> &rLighting ) {
	if ( m_pLighting == rLighting ) {
		return;
	}
	m_pLighting = rLighting;
	m_bModified = true;
}

void Terrain3DRegion::setLocation( const Vector2i &rLocation ) {
	// Locations are bounded by the fixed logical region map.
	if ( Terrain3DData::getRegionMapIndex( rLocation ) < 0 ) {
		LOG( Terrain3D::ERROR, "Location ", rLocation, " out of bounds. Max: ", -TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2, " to ",
			TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2 - 1 );
		return;
	}
	// Marked modified if setting after initialized
	if ( m_location < V2I_MAX && m_location != rLocation ) {
		m_bModified = true;
	}
	SET_IF_DIFF( m_location, rLocation );
	LOG( Terrain3D::DEBUG, "Set location: ", rLocation );
}

Error Terrain3DRegion::save( const String &rPath, const bool b16Bit ) {
	compactDefaultMaps();
	if ( m_bInstanceStoreResident ) {
		m_instanceStore.compactStorage();
	}
	if ( !encodeInstancePage() ) {
		return ERR_INVALID_DATA;
	}
	// Initiate save to external file. The scene will save itself.
	if ( m_location.x == INT32_MAX ) {
		LOG( Terrain3D::ERROR, "Region has not been setup. Location is INT32_MAX. Skipping ", rPath );
	}
	if ( !m_bModified ) {
		LOG( Terrain3D::DEBUG, "Region ", m_location, " not modified. Skipping ", rPath );
		return ERR_SKIP;
	}
	if ( m_pHeightMap.is_null() ) {
		LOG( Terrain3D::ERROR, "Cannot save mapless region ", m_location,
			". Page it through Terrain3DData::ensure_cpu_region_resident() before direct region save." );
		return ERR_UNAVAILABLE;
	}
	if ( rPath.is_empty() && get_path().is_empty() ) {
		LOG( Terrain3D::ERROR, "No valid path provided" );
		return ERR_FILE_NOT_FOUND;
	}
	if ( !rPath.is_empty() ) {
		LOG( Terrain3D::DEBUG, "t3dsetting_t file path for region ", m_location, " to ", rPath );
		take_over_path( rPath );
		// Set region path and take over the path from any other cached resources,
		// incuding those in the undo queue
	}
	LOG( Terrain3D::MESG, "Writing compact R16 region ", m_location, " to ", get_path() );
	setVersion( TERRAIN3D_DATA_CURRENT_VERSION );
	( void ) b16Bit;
	const Error err = ResourceSaver::get_singleton()->save( this, get_path(), ResourceSaver::FLAG_COMPRESS );
	if ( err == OK ) {
		m_bModified = false;
		m_bRuntimeMapsModified = false;
		LOG( Terrain3D::INFO, "File saved successfully" );
	} else {
		LOG( Terrain3D::ERROR, "Cannot save region file: ", get_path(), ". Error code: ", Terrain3D::ERROR,
			". Look up @GlobalScope Error enum in the Godot docs" );
	}
	return err;
}

void Terrain3DRegion::setData( const Dictionary &rData ) {
	// This is a public mutation API, not a deserializer. Route through validated
	// setters and publish one conservative dirty result instead of copying raw
	// internal fields with no downstream contract.
	if ( rData.has( "region_size" ) ) {
		setRegionSize( ( int ) ( rData["region_size"] ) );
	}
	if ( rData.has( "version" ) ) {
		setVersion( ( real_t ) ( rData["version"] ) );
	}
	if ( rData.has( "vertex_spacing" ) ) {
		setVertexSpacing( ( real_t ) ( rData["vertex_spacing"] ) );
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
	m_bModified = true;
}

Dictionary Terrain3DRegion::getData( void ) const {
	Dictionary dict;
	dict["location"] = m_location;
	dict["deleted"] = m_bDeleted;
	dict["edited"] = m_bEdited;
	dict["modified"] = m_bModified;
	dict["version"] = m_fVersion;
	dict["region_size"] = m_nRegionSize;
	dict["vertex_spacing"] = m_fVertexSpacing;
	dict["height_range"] = m_heightRange;
	dict["height_map"] = m_pHeightMap;
	dict["control_map"] = m_pControlMap;
	dict["color_map"] = m_pColorMap;
	dict["channel_flags"] = ( int ) ( m_nChannelFlags );
	dict["instances"] = Dictionary();
	dict["instance_page"] = getInstancePage();
	dict["streamed_nodes"] = m_streamedNodes.duplicate( true );
	dict["lighting"] = m_pLighting;
	return dict;
}

Ref<Terrain3DRegion> Terrain3DRegion::duplicate( const bool bDeep ) {
	Ref<Terrain3DRegion> pRegion;
	pRegion.instantiate();
	pRegion->m_fVersion = m_fVersion;
	pRegion->m_nRegionSize = m_nRegionSize;
	pRegion->m_fVertexSpacing = m_fVertexSpacing;
	pRegion->m_heightRange = m_heightRange;
	pRegion->m_nChannelFlags = m_nChannelFlags;
	pRegion->m_bModified = m_bModified;
	pRegion->m_bDeleted = m_bDeleted;
	pRegion->m_location = m_location;
	pRegion->m_instanceStore = m_instanceStore;
	pRegion->m_instancePage = getInstancePage();
	pRegion->m_bInstanceStoreResident = m_bInstanceStoreResident;
	pRegion->m_bInstanceStoreModified = m_bInstanceStoreModified;
	std::memcpy( pRegion->m_szInstanceMeshCounts, m_szInstanceMeshCounts, sizeof( m_szInstanceMeshCounts ) );
	pRegion->m_nInstanceCount = m_nInstanceCount;
	pRegion->m_nInstanceMeshCount = m_nInstanceMeshCount;
	pRegion->m_streamedNodes = m_streamedNodes.duplicate( true );
	pRegion->rebuildStreamedNodeRuntime();
	pRegion->m_nNextStreamedNodeId = m_nNextStreamedNodeId;
	pRegion->m_streamedNodeCoverageMin = m_streamedNodeCoverageMin;
	pRegion->m_streamedNodeCoverageMax = m_streamedNodeCoverageMax;
	pRegion->m_bStreamedNodeHasExtendedCoverage = m_bStreamedNodeHasExtendedCoverage;
	pRegion->m_pLighting = m_pLighting;
	if ( bDeep ) {
		pRegion->m_pHeightMap = m_pHeightMap.is_valid() ? Ref<Image>( m_pHeightMap->duplicate() ) : Ref<Image>();
		pRegion->m_pControlMap = m_pControlMap.is_valid() ? Ref<Image>( m_pControlMap->duplicate() ) : Ref<Image>();
		pRegion->m_pColorMap = m_pColorMap.is_valid() ? Ref<Image>( m_pColorMap->duplicate() ) : Ref<Image>();
	} else {
		pRegion->m_bEdited = m_bEdited;
		pRegion->m_pHeightMap = m_pHeightMap;
		pRegion->m_pControlMap = m_pControlMap;
		pRegion->m_pColorMap = m_pColorMap;
	}
	return pRegion;
}

void Terrain3DRegion::dump( const bool bVerbose ) const {
	LOG( Terrain3D::MESG, "Region: ", m_location, ", version: ", vformat( "%.2f", m_fVersion ), ", size: ", m_nRegionSize,
		", spacing: ", vformat( "%.1f", m_fVertexSpacing ), ", range: ", vformat( "%.2v", m_heightRange ), ", flags (",
		m_bEdited ? "ed," : "", m_bModified ? "mod," : "", m_bDeleted ? "del" : "", ")" );
	LOG( Terrain3D::MESG, "Instances: mesh groups: ", ( int ) ( m_nInstanceMeshCount ), ", total: ", ( int ) ( m_nInstanceCount ),
		m_bInstanceStoreResident ? ", resident" : ", paged", ", page bytes: ", m_instancePage.size() );
	if ( !m_bInstanceStoreResident ) {
		LOG( Terrain3D::MESG, "Streamed dynamic nodes: ", m_streamedNodes.size() );
		return;
	}
	for ( const t3dinstanceMesh_t &mesh : m_instanceStore.meshes() ) {
		size_t count = 0;
		for ( const t3dinstanceCell_t &cell : mesh.cells ) {
			count += cell.transforms.size();
			if ( bVerbose ) {
				LOG( Terrain3D::MESG, "Mesh ID: ", mesh.id, " cell: ", cell.location, " xforms: ", ( int ) ( cell.transforms.size() ),
					cell.usesColors() ? ", colored" : ", uncolored", cell.bModified ? ", modified" : "" );
			}
		}
		LOG( Terrain3D::MESG, "Mesh ID: ", mesh.id, ", instance count: ", ( int ) ( count ) );
	}
}

/////////////////////
// Protected Functions
/////////////////////

void Terrain3DRegion::_bind_methods( void ) {
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
	BIND_ENUM_CONSTANT( TYPE_MAX );
	BIND_ENUM_CONSTANT( CHANNEL_CONTROL );
	BIND_ENUM_CONSTANT( CHANNEL_COLOR );
	BIND_ENUM_CONSTANT( CHANNEL_DERIVED );
	BIND_ENUM_CONSTANT( CHANNEL_MASK );
	BIND_ENUM_CONSTANT( STREAMED_NODE_BOUNDARY_ANCHOR );
	BIND_ENUM_CONSTANT( STREAMED_NODE_BOUNDARY_COVERAGE );

	ClassDB::bind_method( D_METHOD( "clear" ), &Terrain3DRegion::clear );
	ClassDB::bind_method( D_METHOD( "set_version", "version" ), &Terrain3DRegion::setVersion );
	ClassDB::bind_method( D_METHOD( "get_version" ), &Terrain3DRegion::getVersion );
	ClassDB::bind_method( D_METHOD( "set_region_size", "region_size" ), &Terrain3DRegion::setRegionSize );
	ClassDB::bind_method( D_METHOD( "get_region_size" ), &Terrain3DRegion::getRegionSize );
	ClassDB::bind_method( D_METHOD( "set_vertex_spacing", "vertex_spacing" ), &Terrain3DRegion::setVertexSpacing );
	ClassDB::bind_method( D_METHOD( "get_vertex_spacing" ), &Terrain3DRegion::getVertexSpacing );

	ClassDB::bind_method( D_METHOD( "set_map", "map_type", "map" ), &Terrain3DRegion::setMap );
	ClassDB::bind_method( D_METHOD( "get_map", "map_type" ), &Terrain3DRegion::getMap );
	ClassDB::bind_method( D_METHOD( "set_maps", "maps" ), &Terrain3DRegion::setMaps );
	ClassDB::bind_method( D_METHOD( "get_maps" ), &Terrain3DRegion::getMaps );
	ClassDB::bind_method( D_METHOD( "set_height_map", "map" ), &Terrain3DRegion::setHeightMap );
	ClassDB::bind_method( D_METHOD( "get_height_map" ), &Terrain3DRegion::getHeightMap );
	ClassDB::bind_method( D_METHOD( "set_control_map", "map" ), &Terrain3DRegion::setControlMap );
	ClassDB::bind_method( D_METHOD( "get_control_map" ), &Terrain3DRegion::getControlMap );
	ClassDB::bind_method( D_METHOD( "set_color_map", "map" ), &Terrain3DRegion::setColorMap );
	ClassDB::bind_method( D_METHOD( "get_color_map" ), &Terrain3DRegion::getColorMap );
	ClassDB::bind_method( D_METHOD( "set_channel_flags", "flags" ), &Terrain3DRegion::setChannelFlags );
	ClassDB::bind_method( D_METHOD( "get_channel_flags" ), &Terrain3DRegion::getChannelFlags );
	ClassDB::bind_method( D_METHOD( "has_channel", "map_type" ), &Terrain3DRegion::hasChannel );
	ClassDB::bind_method( D_METHOD( "sanitize_maps" ), &Terrain3DRegion::sanitizeMaps );
	ClassDB::bind_method( D_METHOD( "is_map_default", "map_type" ), &Terrain3DRegion::isMapDefault );
	ClassDB::bind_method( D_METHOD( "compact_map_if_default", "map_type", "mark_modified" ),
		&Terrain3DRegion::compactMapIfDefault, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "compact_default_maps" ), &Terrain3DRegion::compactDefaultMaps );
	ClassDB::bind_method( D_METHOD( "mark_map_edited", "map_type", "recalculate_height_range" ),
						 &Terrain3DRegion::markMapEdited, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "sanitize_map", "map_type", "map" ), &Terrain3DRegion::sanitizeMap );
	ClassDB::bind_method( D_METHOD( "validate_map_size", "map" ), &Terrain3DRegion::validateMapSize );

	ClassDB::bind_method( D_METHOD( "set_height_range", "range" ), &Terrain3DRegion::setHeightRange );
	ClassDB::bind_method( D_METHOD( "get_height_range" ), &Terrain3DRegion::getHeightRange );
	ClassDB::bind_method( D_METHOD( "update_height", "height" ), &Terrain3DRegion::updateHeight );
	ClassDB::bind_method( D_METHOD( "update_heights", "low_high" ), &Terrain3DRegion::updateHeights );
	ClassDB::bind_method( D_METHOD( "calc_height_range" ), &Terrain3DRegion::calcHeightRange );

	ClassDB::bind_method( D_METHOD( "set_instances", "instances" ), &Terrain3DRegion::setInstances );
	ClassDB::bind_method( D_METHOD( "get_instances" ), &Terrain3DRegion::getInstances );
	ClassDB::bind_method( D_METHOD( "_get_instances_legacy_storage" ), &Terrain3DRegion::getInstancesLegacyStorage );
	ClassDB::bind_method( D_METHOD( "set_instance_page", "page" ), &Terrain3DRegion::setInstancePage );
	ClassDB::bind_method( D_METHOD( "get_instance_page" ), &Terrain3DRegion::getInstancePage );
	ClassDB::bind_method( D_METHOD( "ensure_instance_store_resident" ), &Terrain3DRegion::ensureInstanceStoreResident );
	ClassDB::bind_method( D_METHOD( "evict_instance_store" ), &Terrain3DRegion::evictInstanceStore );
	ClassDB::bind_method( D_METHOD( "is_instance_store_resident" ), &Terrain3DRegion::isInstanceStoreResident );
	ClassDB::bind_method( D_METHOD( "get_instance_count", "mesh_id" ), &Terrain3DRegion::getInstanceCount, DEFVAL( -1 ) );
	ClassDB::bind_method( D_METHOD( "get_instance_page_bytes" ), &Terrain3DRegion::getInstancePageBytes );
	ClassDB::bind_method( D_METHOD( "set_streamed_nodes", "nodes" ), &Terrain3DRegion::setStreamedNodes );
	ClassDB::bind_method( D_METHOD( "get_streamed_nodes" ), &Terrain3DRegion::getStreamedNodes );
	ClassDB::bind_method( D_METHOD(
		"add_streamed_node", "scene", "local_transform", "state", "flags", "id",
		"boundary_policy", "coverage_min", "coverage_max", "hlod_min_level"
	), &Terrain3DRegion::addStreamedNode,
		DEFVAL( Dictionary() ), DEFVAL( 0 ), DEFVAL( 0 ), DEFVAL( STREAMED_NODE_BOUNDARY_ANCHOR ),
		DEFVAL( V2I_ZERO ), DEFVAL( V2I_ZERO ), DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD(
		"add_streamed_node_path", "scene_path", "local_transform", "state", "flags", "id",
		"boundary_policy", "coverage_min", "coverage_max", "hlod_min_level"
	), &Terrain3DRegion::addStreamedNodePath,
		DEFVAL( Dictionary() ), DEFVAL( 0 ), DEFVAL( 0 ), DEFVAL( STREAMED_NODE_BOUNDARY_ANCHOR ),
		DEFVAL( V2I_ZERO ), DEFVAL( V2I_ZERO ), DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "remove_streamed_node", "id" ), &Terrain3DRegion::removeStreamedNode );
	ClassDB::bind_method( D_METHOD( "get_streamed_node_count" ), &Terrain3DRegion::getStreamedNodeCount );
	ClassDB::bind_method( D_METHOD( "set_lighting", "lighting" ), &Terrain3DRegion::setLighting );
	ClassDB::bind_method( D_METHOD( "get_lighting" ), &Terrain3DRegion::getLighting );
	ClassDB::bind_method( D_METHOD( "has_lighting" ), &Terrain3DRegion::hasLighting );
	ClassDB::bind_method( D_METHOD( "streamed_node_coverage_intersects", "owner_location", "target_min", "target_max" ),
		&Terrain3DRegion::streamedNodeCoverageIntersects );
	ClassDB::bind_method( D_METHOD( "has_extended_streamed_node_coverage" ),
		&Terrain3DRegion::hasExtendedStreamedNodeCoverage );

	ClassDB::bind_method( D_METHOD( "save", "path", "save_16_bit" ), &Terrain3DRegion::save, DEFVAL( "" ), DEFVAL( false ) );

	ClassDB::bind_method( D_METHOD( "set_deleted", "deleted" ), &Terrain3DRegion::setDeleted );
	ClassDB::bind_method( D_METHOD( "is_deleted" ), &Terrain3DRegion::isDeleted );
	ClassDB::bind_method( D_METHOD( "set_edited", "edited" ), &Terrain3DRegion::setEdited );
	ClassDB::bind_method( D_METHOD( "is_edited" ), &Terrain3DRegion::isEdited );
	ClassDB::bind_method( D_METHOD( "set_modified", "modified" ), &Terrain3DRegion::setModified );
	ClassDB::bind_method( D_METHOD( "is_modified" ), &Terrain3DRegion::isModified );
	ClassDB::bind_method( D_METHOD( "set_location", "location" ), &Terrain3DRegion::setLocation );
	ClassDB::bind_method( D_METHOD( "get_location" ), &Terrain3DRegion::getLocation );

	ClassDB::bind_method( D_METHOD( "get_data" ), &Terrain3DRegion::getData );
	ClassDB::bind_method( D_METHOD( "duplicate", "deep" ), &Terrain3DRegion::duplicate, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "dump", "verbose" ), &Terrain3DRegion::dump, DEFVAL( false ) );

	const int ro_flags = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Identity / Format", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "version", PROPERTY_HINT_NONE, "", ro_flags ), "set_version",
				 "get_version" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_size", PROPERTY_HINT_NONE, "", ro_flags ), "set_region_size",
				 "get_region_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_NONE, "", ro_flags ), "set_vertex_spacing",
				 "get_vertex_spacing" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2, "height_range", PROPERTY_HINT_NONE, "", ro_flags ), "set_height_range",
				 "get_height_range" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "channel_flags", PROPERTY_HINT_FLAGS,
				 "Control,Color,Derived", ro_flags ), "set_channel_flags", "get_channel_flags" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I, "location", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				 "set_location", "get_location" );

	ADD_GROUP( "Maps", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "height_map", PROPERTY_HINT_RESOURCE_TYPE, "Image", ro_flags ),
				 "set_height_map", "get_height_map" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "control_map", PROPERTY_HINT_RESOURCE_TYPE, "Image", ro_flags ),
				 "set_control_map", "get_control_map" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "color_map", PROPERTY_HINT_RESOURCE_TYPE, "Image", ro_flags ),
				 "set_color_map", "get_color_map" );

	ADD_GROUP( "Instances", "" );
	ADD_PROPERTY( PropertyInfo( Variant::DICTIONARY, "instances", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
				 "set_instances", "_get_instances_legacy_storage" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instance_page", PROPERTY_HINT_NONE, "", ro_flags ),
		"set_instance_page", "get_instance_page" );

	ADD_GROUP( "Streamed Nodes", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "streamed_nodes", PROPERTY_HINT_NONE, "", ro_flags ),
		"set_streamed_nodes", "get_streamed_nodes" );

	ADD_GROUP( "Baked Lighting", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "lighting", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DRegionLighting", ro_flags ),
		"set_lighting", "get_lighting" );

	ADD_GROUP( "Runtime / Edit State", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "edited", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "set_edited",
				 "is_edited" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "deleted", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "set_deleted",
				 "is_deleted" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "modified", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "set_modified",
				 "is_modified" );

}
