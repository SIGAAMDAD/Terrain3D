// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable.hpp>

#include <algorithm>
#include "eastl_config.h"
#include <EASTL/array.h>

#include "Terrain3DConstants.h"
#include "core/logger.h"
#include "terrain_3d.h"
#include "data/terrain_3d_data.h"
#include "rendering/terrain_3d_compute.h"
#include "authoring/terrain_3d_editor.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_sampler.h"
#include "core/terrain_3d_util.h"

namespace {


typedef enum t3dEditSnapshotKind_e {
	EDIT_SNAPSHOT_MAP = 0,
	EDIT_SNAPSHOT_INSTANCES = 1,
} t3dEditSnapshotKind_t;

#define TERRAIN3DEDITORQUANTIZERGBA8( value ) \
	( uint32_t ) ( CLAMP( Math::round( ( value ) * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) )

} // namespace

///////////////////////////
// Private Functions
///////////////////////////

bool T3DEditor::isBrushSlopeAllowed(
	T3DSampler &rSampler, const bool bUnrestricted,
	const Vector2 &vSlopeRange, const Vector3 &rPosition
) {
	if ( bUnrestricted ) {
		return true;
	}
	uint32_t control = UINT32_MAX;
	Vector3 normal = V3_NAN;
	if ( !rSampler.sampleControlAndSlopeNormal( rPosition, control, normal ) ) {
		return false;
	}
	const real_t fDegrees = Math::rad_to_deg( Math::acos( CLAMP( normal.dot( V3_UP ), -1.0f, 1.0f ) ) );
	return vSlopeRange.x <= fDegrees && fDegrees <= vSlopeRange.y;
}

uint32_t T3DEditor::packColorRgba8( const Color &rColor ) {
	return TERRAIN3DEDITORQUANTIZERGBA8( rColor.r ) |
		( TERRAIN3DEDITORQUANTIZERGBA8( rColor.g ) << 8u ) |
		( TERRAIN3DEDITORQUANTIZERGBA8( rColor.b ) << 16u ) |
		( TERRAIN3DEDITORQUANTIZERGBA8( rColor.a ) << 24u );
}

Color T3DEditor::unpackColorRgba8( const uint32_t nValue ) {
	return Color( ( real_t ) ( nValue & 0xffu ) / TERRAIN3D_BYTE_MAX_FLOAT, ( real_t ) ( ( nValue >> 8u ) & 0xffu ) / TERRAIN3D_BYTE_MAX_FLOAT,
		( real_t ) ( ( nValue >> 16u ) & 0xffu ) / TERRAIN3D_BYTE_MAX_FLOAT, ( real_t ) ( ( nValue >> 24u ) & 0xffu ) / TERRAIN3D_BYTE_MAX_FLOAT );
}

uint32_t T3DEditor::readU32( const uint8_t *pBytes, const size_t iIndex ) {
	uint32_t value = TERRAIN3D_REGION_DEFAULT_CONTROL;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + iIndex * sizeof( uint32_t ), sizeof( uint32_t ) );
	}
	return value;
}

Color T3DEditor::readMapPixel(
	const t3dRegionMapCache_t &rEntry, const size_t iIndex,
	const Vector2i &rPixel, const t3dMapType_t mapType
) {
	if ( !rEntry.bDirectAccess || !rEntry.pMapBytes ) {
		const Color raw = rEntry.pMap->get_pixelv( rPixel );
		return mapType == T3DRegion::TYPE_HEIGHT
			? Color( T3DRegionCodec::readHeightPixel( rEntry.pMap, rPixel ), 0.0f, 0.0f, 1.0f )
			: raw;
	}
	if ( mapType == T3DRegion::TYPE_HEIGHT ) {
		uint16_t encoded = 0u;
		std::memcpy( &encoded, rEntry.pMapBytes + iIndex * sizeof( uint16_t ), sizeof( uint16_t ) );
		return Color( T3DRegionCodec::decodeHeightU16( encoded ), 0.0f, 0.0f, 1.0f );
	}
	if ( mapType == T3DRegion::TYPE_CONTROL ) {
		return Color( asFloat( readU32( rEntry.pMapBytes, iIndex ) ), 0.0f, 0.0f, 1.0f );
	}
	const uint8_t *pRgba = rEntry.pMapBytes + iIndex * 4u;
	return Color( ( real_t ) ( pRgba[0] ) / TERRAIN3D_BYTE_MAX_FLOAT, ( real_t ) ( pRgba[1] ) / TERRAIN3D_BYTE_MAX_FLOAT, ( real_t ) ( pRgba[2] ) / TERRAIN3D_BYTE_MAX_FLOAT,
		( real_t ) ( pRgba[3] ) / TERRAIN3D_BYTE_MAX_FLOAT );
}

uint32_t T3DEditor::readControlBits(
	const t3dRegionMapCache_t &rEntry, const size_t iIndex,
	const Vector2i &rPixel
) {
	if ( rEntry.pControlBytes ) {
		return readU32( rEntry.pControlBytes, iIndex );
	}
	return rEntry.pControlMap ? asUint( rEntry.pControlMap->get_pixelv( rPixel ).r )
		: TERRAIN3D_REGION_DEFAULT_CONTROL;
}

void T3DEditor::writeMapPixel(
	t3dRegionMapCache_t &rEntry, const size_t iIndex, const Vector2i &rPixel,
	const Color &clrValue, const t3dMapType_t mapType
) {
	if ( !rEntry.bDirectAccess || !rEntry.pMapBytes ) {
		if ( mapType == T3DRegion::TYPE_HEIGHT ) {
			T3DRegionCodec::writeHeightPixel( rEntry.pMap, rPixel, clrValue.r );
		} else {
			rEntry.pMap->set_pixelv( rPixel, clrValue );
		}
		return;
	}
	if ( mapType == T3DRegion::TYPE_HEIGHT ) {
		const uint16_t encoded = T3DRegionCodec::encodeHeightU16( clrValue.r );
		std::memcpy( rEntry.pMapBytes + iIndex * sizeof( uint16_t ), &encoded, sizeof( uint16_t ) );
		return;
	}
	if ( mapType == T3DRegion::TYPE_CONTROL ) {
		const uint32_t bits = asUint( clrValue.r );
		std::memcpy( rEntry.pMapBytes + iIndex * sizeof( uint32_t ), &bits, sizeof( uint32_t ) );
		return;
	}
	uint8_t *pRgba = rEntry.pMapBytes + iIndex * 4u;
	pRgba[0] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.r ) );
	pRgba[1] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.g ) );
	pRgba[2] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.b ) );
	pRgba[3] = ( uint8_t ) ( TERRAIN3DEDITORQUANTIZERGBA8( clrValue.a ) );
}

bool T3DEditor::locationLess( const Vector2i &vA, const Vector2i &rB ) {
	return vA.x == rB.x ? vA.y < rB.y : vA.x < rB.x;
}

void T3DEditor::updateBrushAlphaCache(
	Image *const pBrushImage, const Vector2i &vImageSize,
	const real_t fGamma
) {
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
			m_fBrushAlphaCache[( size_t ) ( y * vImageSize.x + x )] = std::isnan( fAlpha ) ? 0.f : ( float ) ( fAlpha );
		}
	}
}

Dictionary T3DEditor::captureEditSnapshot(
	const Ref<T3DRegion> &rRegion,
	const Dictionary &rTemplate,
	const bool bMapWasImplicit
) const {
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

	const t3dMapType_t map_type = rTemplate.has( "map_type" ) ? ( t3dMapType_t ) ( ( int ) ( rTemplate["map_type"] ) ) : getMapType();
	if ( map_type < T3DRegion::TYPE_HEIGHT || map_type >= T3DRegion::TYPE_MAX ) {
		return Dictionary();
	}
	snapshot["map_type"] = ( int ) ( map_type );
	if ( bMapWasImplicit && ( map_type == T3DRegion::TYPE_CONTROL || map_type == T3DRegion::TYPE_COLOR ) ) {
		snapshot["map_absent"] = true;
		return snapshot;
	}
	const Ref<Image> pMap = rRegion->getMap( map_type );
	if ( pMap.is_null() ) {
		return Dictionary();
	}

	snapshot["map"] = pMap->duplicate();
	if ( map_type == T3DRegion::TYPE_HEIGHT ) {
		snapshot["height_range"] = rRegion->getHeightRange();
	}
	return snapshot;
}

void T3DEditor::applyEditSnapshot(
	const Dictionary &rSnapshot,
	eastl::array<eastl::vector<Vector2i>, T3DRegion::TYPE_MAX> &rMapUpdates,
	bool &bInstancesChanged
) {
	if ( !rSnapshot.has( "kind" ) || !rSnapshot.has( "location" ) ) {
		return;
	}

	T3DData *const pData = m_pTerrain->getData();
	const Vector2i location = rSnapshot["location"];
	Ref<T3DRegion> pRegion = pData->getRegion( location );
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
	if ( map_type < T3DRegion::TYPE_HEIGHT || map_type >= T3DRegion::TYPE_MAX ) {
		return;
	}
	if ( ( bool ) ( rSnapshot.get( "map_absent", false ) ) ) {
		if ( map_type == T3DRegion::TYPE_CONTROL || map_type == T3DRegion::TYPE_COLOR ) {
			pRegion->setMap( map_type, Ref<Image>() );
			pRegion->setModified( true );
			rMapUpdates[( size_t ) ( map_type )].push_back( location );
		}
		return;
	}
	if ( !rSnapshot.has( "map" ) ) {
		return;
	}
	const Ref<Image> pSnapshotMap = rSnapshot["map"];
	if ( pSnapshotMap.is_null() ) {
		return;
	}

	const Ref<Image> pRestoredMap = pSnapshotMap->duplicate();
	pRegion->setMap( map_type, pRestoredMap );
	if ( map_type == T3DRegion::TYPE_HEIGHT && rSnapshot.has( "height_range" ) ) {
		pRegion->setHeightRange( rSnapshot["height_range"] );
	}
	pRegion->setModified( true );
	rMapUpdates[( size_t ) ( map_type )].push_back( location );
}

// Sends the whole region aabb to edited_area
void T3DEditor::sendRegionAabb( const Vector2i &rRegionLoc, const Vector2 &rHeightRange ) {
	const T3DTerrain::t3dRegionSize_t region_size = m_pTerrain->getRegionSize();
	AABB edited_area;
	edited_area.position = Vector3( rRegionLoc.x * region_size, rHeightRange.x, rRegionLoc.y * region_size );
	edited_area.size = Vector3( region_size, rHeightRange.y - rHeightRange.x, region_size );
	edited_area.position *= m_pTerrain->getVertexSpacing();
	edited_area.size *= m_pTerrain->getVertexSpacing();
	m_pTerrain->getData()->addEditedArea( edited_area );
}

// Process location to add new region, mark as deleted, or just retrieve
Ref<T3DRegion> T3DEditor::operateRegion( const Vector2i &rRegionLoc ) {
	bool bChanged = false;
	Vector2 height_range;
	T3DData *const pData = m_pTerrain->getData();
	// Check if in bounds, limiting errors
	bool bCanPrint = false;
	const uint64_t ticks = Time::get_singleton()->get_ticks_msec();
	if ( ticks - m_nLastRegionBoundsError > 1000 ) {
		m_nLastRegionBoundsError = ticks;
		bCanPrint = true;
	}
	if ( !pData->isRegionLocationValidInternal( rRegionLoc ) ) {
		if ( bCanPrint ) {
			LOG( T3DTerrain::INFO, "Location ", rRegionLoc, " out of bounds. Max: ", -pData->getRegionMapSizeInternal() / 2,
				" to ", pData->getRegionMapSizeInternal() / 2 - 1 );
		}
		return Ref<T3DRegion>();
	}

	// Get Region & dump data if debug
	Ref<T3DRegion> pRegion = pData->getRegion( rRegionLoc );
	if ( pRegion.is_valid() && !pRegion->isDeleted() && !pData->ensureCpuRegionResident( rRegionLoc ) ) {
		LOG( T3DTerrain::ERROR, "Unable to page CPU terrain maps for editor region ", rRegionLoc );
		return Ref<T3DRegion>();
	}
	if ( bCanPrint ) {
		LOG( T3DTerrain::DEBUG, "t3dTool_t: ", m_tool, " Op: ", m_operation, " processing region ", rRegionLoc, ": ",
			ptrToStr( *pRegion ) );
	}

	// Create new region if location is null or deleted
	if ( pRegion.is_null() || ( pRegion.is_valid() && pRegion->isDeleted() ) ) {
		// And tool is Add Region, or Height + auto_regions
		if ( ( m_tool == REGION && m_operation == ADD ) ||
			( ( m_tool == SCULPT || m_tool == HEIGHT ) && m_brushData["auto_regions"] ) ) {
			if ( !m_undoData.has( "region_locations" ) ) {
				m_undoData["region_locations"] = pData->getRegionLocations().duplicate();
			}
			LOG( T3DTerrain::DEBUG, "Adding blank region at: ", rRegionLoc, ", ptr: ", ptrToStr( *pRegion ) );
			pRegion = pData->addRegionBlank( rRegionLoc );
			if ( pRegion.is_null() ) {
				LOG( T3DTerrain::ERROR, "A new region cannot be created" );
				return pRegion;
			}
			m_editedRegions.push_back( pRegion ); // Ensure new region is added to the redo set
			bChanged = true;
		}
	}

	// If removing region
	else if ( pRegion.is_valid() && m_tool == REGION && m_operation == SUBTRACT ) {
		if ( !m_undoData.has( "region_locations" ) ) {
			m_undoData["region_locations"] = pData->getRegionLocations().duplicate();
		}
		LOG( T3DTerrain::DEBUG, "Removing region at: ", rRegionLoc, ", ptr: ", ptrToStr( *pRegion ) );
		m_originalRegions.push_back( pRegion );
		height_range = pRegion->getHeightRange();
		m_pTerrain->getData()->removeRegion( pRegion );
		bChanged = true;
	}

	if ( bChanged ) {
		m_addedRemovedLocations.push_back( rRegionLoc );
		pRegion->setModified( true );
		sendRegionAabb( rRegionLoc, height_range );
	}
	return pRegion;
}

void T3DEditor::operateMap( const Vector3 &rGlobalPosition, const real_t fCameraDirection ) {
	LOG( T3DTerrain::EXTREME, "Operating at ", rGlobalPosition, " tool type ", m_tool, " op ", m_operation );

	const t3dMapType_t map_type = getMapType();
	if ( map_type == T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Invalid tool selected" );
		return;
	}

	const int region_size = m_pTerrain->getRegionSize();
	const Vector2i region_vsize = V2I( region_size );

	// If no region and can't add one, skip whole function. Checked again later
	T3DData *const pData = m_pTerrain->getData();
	if ( !pData->hasRegionp( rGlobalPosition ) &&
		( !m_brushData["auto_regions"] || ( m_tool != SCULPT && m_tool != HEIGHT ) ) ) {
		return;
	}

	const bool bModifierAlt = m_brushData["modifier_alt"];
	// bool modifier_shift = _brush_data["modifier_shift"];

	Image *const pBrushImage = cast_to<Image>( m_brushData["brush_image"] );
	if ( !pBrushImage ) {
		LOG( T3DTerrain::ERROR, "Invalid brush image. Returning" );
		return;
	}
	const Vector2i img_size = m_brushData["brush_image_size"];
	const real_t fBrushSize = CLAMP( ( real_t ) ( m_brushData.get( "size", 10.f ) ), 0.1f, 4096.f ); // Meters
	// Typicall we multiply mouse pressure & strength setting, but
	// * Mouse movement w/ button down has a pressure of 1
	// * Mouse clicks always have pressure of 0
	// * Pen movement pressure varies, sometimes lifting or clicking has a pressure of 0
	// If we're operating with a pressure of 0.001-.999 it's a pen
	// So if there's a 0 pressure operation >100ms after a pen operation, we assume it's
	// a mouse click. This occasionally catches a pen click, but avoids most pen lifts.
	real_t fMousePressure = CLAMP( ( real_t ) ( m_brushData.get( "mouse_pressure", 0.f ) ), 0.f, 1.f );
	if ( fMousePressure > CMP_EPSILON && fMousePressure < 1.f ) {
		m_lastPenTick = Time::get_singleton()->get_ticks_msec();
	}
	const uint64_t ticks = Time::get_singleton()->get_ticks_msec();
	if ( fMousePressure < CMP_EPSILON && ticks - m_lastPenTick >= 100 ) {
		fMousePressure = 1.f;
	}
	const real_t strength = fMousePressure * ( real_t )m_brushData["strength"];

	const real_t fHeight = m_brushData["height"];
	const Color color = m_brushData["color"];
	const real_t roughness = m_brushData["roughness"];

	const bool bEnableTexture = m_brushData["enable_texture"];
	const bool bTextureFilter = m_brushData["texture_filter"];
	const int margin = m_brushData["margin"];
	const int asset_id = m_brushData["asset_id"];
	const Ref<T3DAssets> pAssets = m_pTerrain->getAssets();
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
	const bool bEnableAngle = m_brushData["enable_angle"];
	const bool bDynamicAngle = m_brushData["dynamic_angle"];
	real_t angle = m_brushData["angle"];

	const bool bEnableScale = m_brushData["enable_scale"];
	const real_t fScale = m_brushData["scale"];

	const real_t fGamma = m_brushData["gamma"];
	PackedVector3Array gradient_points = m_brushData["gradient_points"];

	const real_t randf = UtilityFunctions::randf();
	real_t fRot = randf * Math_PI * ( real_t ) ( m_brushData["brush_spin_speed"] );
	if ( m_brushData["align_to_view"] ) {
		fRot += fCameraDirection;
	}
	// Rotate the decal to align with the brush
	if ( m_pTerrain->getPlugin() ) {
		Node *const pNode = cast_to<Node>( m_pTerrain->getPlugin()->get( "ui" ) );
		if ( pNode->has_method( "set_decal_rotation" ) ) {
			pNode->call( "set_decal_rotation", fRot );
		}
	}
	AABB edited_area;
	edited_area.position = rGlobalPosition - Vector3( fBrushSize, 0.f, fBrushSize ) * .5f;
	edited_area.size = Vector3( fBrushSize, 0.f, fBrushSize );

	if ( m_tool == INSTANCER ) {
		if ( m_operation == SUBTRACT ) {
			m_pTerrain->getInstancer()->removeInstances( rGlobalPosition, m_brushData );
		} else if ( m_operation == ADD ) {
			m_pTerrain->getInstancer()->addInstances( rGlobalPosition, m_brushData );
		}
		return;
	}

	// MAP Operations
	const real_t fVertexSpacing = m_pTerrain->getVertexSpacing();
	const real_t fHalfBrushSize = fBrushSize * .5f;
	const real_t fSampleCenterOffset = fVertexSpacing * .5f;
	const int32_t sample_count = MAX( 1, ( int32_t )Math::ceil( fBrushSize / fVertexSpacing ) );
	const int64_t total_samples = ( int64_t ) ( sample_count ) * ( int64_t ) ( sample_count );
	const int64_t brush_pixel_count = ( int64_t ) ( img_size.x ) * ( int64_t ) ( img_size.y );
	const bool bUseAlphaCache = total_samples >= std::max<int64_t>( 4096, brush_pixel_count / 4 );
	if ( bUseAlphaCache ) {
		updateBrushAlphaCache( pBrushImage, img_size, fGamma );
	}
	T3DSampler brushSampler( pData->getSamplingSourceInternal() );
	const Vector2 clamped_slope_range = CLAMP( slope_range, V2_ZERO, V2( 90.f ) );
	const bool bUnrestrictedSlope = clamped_slope_range.y - clamped_slope_range.x > 89.99f;

	const bool bComputeSupported =
		m_operation != AVERAGE &&
		( ( map_type == T3DRegion::TYPE_HEIGHT && ( m_operation == ADD || m_operation == SUBTRACT || m_operation == GRADIENT ) ) ||
		 ( map_type == T3DRegion::TYPE_CONTROL &&
		  ( m_tool == TEXTURE || m_tool == AUTOSHADER || m_tool == HOLES || m_tool == NAVIGATION ) &&
		  ( m_operation == ADD || m_operation == SUBTRACT || m_operation == REPLACE ) ) ||
		 ( map_type == T3DRegion::TYPE_COLOR && ( m_tool == COLOR || m_tool == ROUGHNESS ) &&
		  ( m_operation == ADD || m_operation == SUBTRACT ) ) );
	T3DCompute *const pComputeBackend = pData->getComputeBackend();
	const bool bUseCompute =
		bComputeSupported && total_samples >= 4096 && pComputeBackend && pComputeBackend->isAvailable();
	// Average operations intentionally observe earlier writes in the same stamp.
	// Other tools can edit detached byte planes and publish each region once,
	// eliminating thousands of Image::get_pixelv/set_pixelv calls.
	const bool bUseDirectMapAccess = m_operation != AVERAGE && ( bUseCompute || total_samples >= 1024 );

	m_xAxisScratch.resize( ( size_t ) ( sample_count ) );
	m_zAxisScratch.resize( ( size_t ) ( sample_count ) );
	eastl::vector<t3dAxisSample_t> &x_samples = m_xAxisScratch;
	eastl::vector<t3dAxisSample_t> &z_samples = m_zAxisScratch;
	for ( int32_t i = 0; i < sample_count; ++i ) {
		const real_t fDistance = ( real_t ) ( i ) * fVertexSpacing;
		const bool bSingleSample = sample_count == 1;
		const real_t offset = bSingleSample ? 0.0f : fDistance - fHalfBrushSize + fSampleCenterOffset;
		const real_t fGlobalX = rGlobalPosition.x + offset;
		const real_t fGlobalZ = rGlobalPosition.z + offset;
		const real_t fDescaledX = fGlobalX / fVertexSpacing;
		const real_t fDescaledZ = fGlobalZ / fVertexSpacing;
		const int32_t region_x = ( int32_t )Math::floor( fDescaledX / ( real_t ) ( region_size ) );
		const int32_t region_z = ( int32_t )Math::floor( fDescaledZ / ( real_t ) ( region_size ) );
		x_samples[i] = {offset, fGlobalX, region_x, ( int32_t )( fDescaledX - ( real_t ) ( region_x * region_size ) ),
						offset / fBrushSize};
		z_samples[i] = {offset, fGlobalZ, region_z, ( int32_t )( fDescaledZ - ( real_t ) ( region_z * region_size ) ),
						offset / fBrushSize};
	}

	const real_t fRotCos = Math::cos( fRot );
	const real_t fRotSin = Math::sin( fRot );
	const eastl::array<uint32_t, 8> scale_align = {5, 6, 7, 0, 1, 2, 3, 4};

	// Resolve each covered region and map once. At maximum brush size, repeatedly resolving regions
	// per column still costs tens of thousands of dictionary lookups despite touching very few regions.
	const int32_t min_region_x = x_samples.front().regionLocation;
	const int32_t max_region_x = x_samples.back().regionLocation;
	const int32_t min_region_z = z_samples.front().regionLocation;
	const int32_t max_region_z = z_samples.back().regionLocation;
	const int32_t region_cache_width = max_region_x - min_region_x + 1;
	const int32_t region_cache_height = max_region_z - min_region_z + 1;
	m_regionCacheScratch.clear();
	m_regionCacheScratch.resize( ( size_t ) ( region_cache_width ) * ( size_t ) ( region_cache_height ) );
	eastl::vector<t3dRegionMapCache_t> &region_cache = m_regionCacheScratch;
	// Store regions in the same Z-major/X-minor order used by Image pixels and by
	// the brush traversal below. Crossing an X region boundary therefore advances
	// to an adjacent cache entry instead of jumping by region_cache_height.
	for ( int32_t region_z = min_region_z; region_z <= max_region_z; ++region_z ) {
		for ( int32_t region_x = min_region_x; region_x <= max_region_x; ++region_x ) {
			t3dRegionMapCache_t &entry = region_cache[( size_t ) ( region_z - min_region_z ) * ( size_t ) ( region_cache_width ) +
											 ( size_t ) ( region_x - min_region_x )];
			entry.location = Vector2i( region_x, region_z );
			entry.pRegion = operateRegion( entry.location );
			entry.bMapWasImplicit = entry.pRegion.is_valid() && entry.pRegion->getMapPtr( map_type ) == nullptr;
			entry.pMap = entry.pRegion.is_valid() ? entry.pRegion->getMapPtrForWrite( map_type ) : nullptr;
			entry.pControlMap = entry.pRegion.is_valid() ? entry.pRegion->getMapPtr( T3DRegion::TYPE_CONTROL ) : nullptr;
			if ( !entry.pMap || !bUseDirectMapAccess ) {
				continue;
			}
			entry.mapFormat = entry.pMap->get_format();
			entry.bMapHasMipmaps = entry.pMap->has_mipmaps();
			const Image::Format expected_format = map_type == T3DRegion::TYPE_HEIGHT ? Image::FORMAT_R16
				: map_type == T3DRegion::TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8;
			const size_t bytes_per_texel = map_type == T3DRegion::TYPE_HEIGHT ? sizeof( uint16_t ) : sizeof( uint32_t );
			const size_t expected_bytes = ( size_t ) ( region_size ) * ( size_t ) ( region_size ) * bytes_per_texel;
			if ( entry.mapFormat != expected_format || entry.pMap->get_width() != region_size ||
				entry.pMap->get_height() != region_size ) {
				continue;
			}
			entry.mapData = entry.pMap->get_data();
			if ( ( size_t ) ( entry.mapData.size() ) < expected_bytes ) {
				entry.mapData.clear();
				continue;
			}
			entry.pMapBytes = entry.mapData.ptrw();
			entry.bDirectAccess = entry.pMapBytes != nullptr;
			if ( !entry.bDirectAccess ) {
				continue;
			}
			if ( map_type == T3DRegion::TYPE_CONTROL ) {
				entry.pControlBytes = entry.pMapBytes;
			} else if ( entry.pControlMap && entry.pControlMap->get_format() == Image::FORMAT_RF &&
				entry.pControlMap->get_width() == region_size && entry.pControlMap->get_height() == region_size ) {
				entry.controlData = entry.pControlMap->get_data();
				if ( ( size_t ) ( entry.controlData.size() ) >=
					( size_t ) ( region_size ) * ( size_t ) ( region_size ) * sizeof( uint32_t ) ) {
					entry.pControlBytes = entry.controlData.ptr();
				}
			}
		}
	}

	m_computeElementScratch.clear();
	m_computeWriteScratch.clear();
	eastl::vector<T3DCompute::t3dBrushElement_t> &compute_elements = m_computeElementScratch;
	eastl::vector<t3dComputeWrite_t> &compute_writes = m_computeWriteScratch;
	if ( bUseCompute ) {
		compute_elements.reserve( ( size_t ) ( total_samples ) );
		compute_writes.reserve( ( size_t ) ( total_samples ) );
	}


	real_t fComputeAngle = angle;
	if ( bDynamicAngle ) {
		fComputeAngle = Vector2( -m_operationMovement.x, m_operationMovement.z ).angle();
		fComputeAngle = ( real_t ) ( Math::fmod( Math::rad_to_deg( fComputeAngle ) + 450.f, ( real_t ) ( 360.f ) ) );
	}
	const uint32_t compute_uv_rotation = ( uint32_t ) ( CLAMP( Math::round( fComputeAngle / 22.5f ), 0.f, 15.f ) );
	const uint32_t compute_uv_scale = scale_align[( uint8_t ) ( CLAMP( Math::round( ( fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];

	// Terrain image rows are X-contiguous. Keep X in the inner loop so ordinary
	// stamps stream through map cachelines rather than revisiting one cacheline per
	// column after walking the full brush height.
	const real_t fCenteredUvStep = fVertexSpacing / fBrushSize;
	const real_t fRotatedUStep = fCenteredUvStep * fRotCos;
	const real_t fRotatedVStep = fCenteredUvStep * fRotSin;
	for ( const t3dAxisSample_t &z_sample : z_samples ) {
		real_t fRowU = x_samples.front().fCenteredUv * fRotCos - z_sample.fCenteredUv * fRotSin + .5f;
		real_t fRowV = x_samples.front().fCenteredUv * fRotSin + z_sample.fCenteredUv * fRotCos + .5f;
		for ( const t3dAxisSample_t &x_sample : x_samples ) {
			const real_t fRotatedU = CLAMP( fRowU, 0.f, 1.f );
			const real_t fRotatedV = CLAMP( fRowV, 0.f, 1.f );
			fRowU += fRotatedUStep;
			fRowV += fRotatedVStep;
			const Vector2i brush_pixel_position = Vector2i( fRotatedU * img_size.x, fRotatedV * img_size.y );
			if ( !isInBounds( brush_pixel_position, img_size ) ) {
				continue;
			}

			real_t fBrushAlpha;
			if ( bUseAlphaCache ) {
				fBrushAlpha = m_fBrushAlphaCache[( size_t ) ( brush_pixel_position.y * img_size.x + brush_pixel_position.x )];
			} else {
				fBrushAlpha = pBrushImage->get_pixelv( brush_pixel_position ).r;
				fBrushAlpha = ( real_t ) ( Math::pow( ( double ) ( fBrushAlpha ), ( double ) ( fGamma ) ) );
				fBrushAlpha = std::isnan( fBrushAlpha ) ? 0.f : fBrushAlpha;
			}
			if ( fBrushAlpha <= CMP_EPSILON ) {
				continue;
			}

			const size_t region_cache_index =
				( size_t ) ( z_sample.regionLocation - min_region_z ) * ( size_t ) ( region_cache_width ) +
				( size_t ) ( x_sample.regionLocation - min_region_x );
			t3dRegionMapCache_t &region_entry = region_cache[region_cache_index];
			if ( !region_entry.pMap ) {
				continue;
			}
			Ref<T3DRegion> &region = region_entry.pRegion;

			const Vector2i mapPixelPosition( x_sample.mapPixel, z_sample.mapPixel );
			if ( !isInBounds( mapPixelPosition, region_vsize ) ) {
				continue;
			}

			const Vector2 brushOffset( x_sample.fOffset, z_sample.fOffset );
			const Vector3 brushGlobalPosition(
				x_sample.fGlobalPosition, rGlobalPosition.y,
				z_sample.fGlobalPosition
			);
			Vector3 edited_position = brushGlobalPosition;
			if ( map_type != T3DRegion::TYPE_HEIGHT ) {
				edited_position.y = brushSampler.sampleHeight( edited_position );
				edited_area = edited_area.expand( edited_position );
			}

			const size_t map_index =
				( size_t ) ( mapPixelPosition.y ) * ( size_t ) ( region_size ) + ( size_t ) ( mapPixelPosition.x );
			const Color src = readMapPixel( region_entry, map_index, mapPixelPosition, map_type );
			Color dest = src;
			if ( bUseCompute ) {
				bool bEligible = true;
				uint32_t source_value = 0u;
				uint32_t target_value = 0u;
				if ( map_type == T3DRegion::TYPE_HEIGHT ) {
					const float fSourceHeight = std::isfinite( src.r ) ? src.r : 0.0f;
					source_value = asUint( fSourceHeight );
					edited_position.y = fSourceHeight;
					edited_area = edited_area.expand( edited_position );
					if ( m_operation == GRADIENT ) {
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
								const real_t fWeight = CLAMP(
									direction.normalized().dot( brushXz - point1Xz ) / direction.length(), 0.0f, 1.0f );
								target_value = asUint( ( float ) ( Math::lerp( point_1.y, point_2.y, fWeight ) ) );
							}
						}
					}
				} else if ( map_type == T3DRegion::TYPE_CONTROL ) {
					source_value = asUint( src.r );
					if ( m_tool == TEXTURE ) {
						bEligible = isBrushSlopeAllowed( brushSampler, bUnrestrictedSlope, clamped_slope_range, brushGlobalPosition );
					}
				} else {
					source_value = packColorRgba8( src );
					if ( bTextureFilter ) {
						const float fControl = asFloat( readControlBits( region_entry, map_index, mapPixelPosition ) );
						const int texture_id =
							( getBlend( fControl ) > 110 - margin ) ? getOverlay( fControl ) : getBase( fControl );
						bEligible = texture_id == asset_id;
					}
					if ( bEligible ) {
						bEligible = isBrushSlopeAllowed( brushSampler, bUnrestrictedSlope, clamped_slope_range, brushGlobalPosition );
					}
				}
				if ( bEligible ) {
					T3DCompute::t3dBrushElement_t element;
					element.value = source_value;
					element.alpha = asUint( ( float ) ( fBrushAlpha ) );
					element.target = target_value;
					element.flags = 1u;
					compute_elements.push_back( element );
					compute_writes.push_back( {region_cache_index, mapPixelPosition, source_value, edited_position} );
				}
				continue;
			}
			if ( map_type == T3DRegion::TYPE_HEIGHT ) {
				real_t fSrcf = src.r;
				// In case data in existing map has nan or inf saved, check, and reset to real number if required.
				fSrcf = std::isnan( fSrcf ) ? 0.f : fSrcf;
				edited_position.y = fSrcf;
				edited_area = edited_area.expand( edited_position );
				real_t fDestf = fSrcf;

				switch ( m_operation ) {
					case ADD: {
						if ( m_tool == HEIGHT ) {
							// Height
							fDestf = Math::lerp( fSrcf, fHeight, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
						} else if ( bModifierAlt && !std::isnan( rGlobalPosition.y ) ) {
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
						} else if ( bModifierAlt && !std::isnan( rGlobalPosition.y ) ) {
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
						const real_t fAvgDefault = m_pTerrain->getMaterial()->getWorldBackground() == 0u ? fSrcf : 0.f;
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
								return;
							}
							Vector2 brushXz = Vector2( brushGlobalPosition.x, brushGlobalPosition.z );

							if ( m_operationMovement.length_squared() > 0.f ) {
								// Ramp up/down only in the direction of movement, to avoid giving winding
								// paths one edge higher than the other.
								const Vector2 movement_xz =
									Vector2( m_operationMovement.x, m_operationMovement.z ).normalized();
								const Vector2 offset = movement_xz * Vector2( brushOffset ).dot( movement_xz );
								brushXz = Vector2( rGlobalPosition.x + offset.x, rGlobalPosition.z + offset.y );
							}

							real_t fWeight = dir.normalized().dot( brushXz - point1Xz ) / dir.length();
							fWeight = Math::clamp( fWeight, ( real_t )0.0f, ( real_t )1.0f );
							const real_t fGradientHeight = Math::lerp( point_1.y, point_2.y, fWeight );
							fDestf = Math::lerp( fSrcf, fGradientHeight, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
						}
						break;
					}
					default:
						break;
				}
				dest = Color( fDestf, 0.f, 0.f, 1.f );
				region->updateHeight( fDestf );
				pData->updateMasterHeight( fDestf );
				edited_position.y = fDestf;
				edited_area = edited_area.expand( edited_position );

			} else if ( map_type == T3DRegion::TYPE_CONTROL ) {
				// Get current bit field from pixel
				uint32_t base_id = getBase( src.r );
				uint32_t overlay_id = getOverlay( src.r );
				real_t fBlend = ( real_t ) ( getBlend( src.r ) ) / 255.f;
				uint32_t uvrotation = getUvRotation( src.r );
				uint32_t uvscale = getUvScale( src.r );
				bool bHole = isHole( src.r );
				bool bNavigation = isNav( src.r );
				bool bAutoshader = isAuto( src.r );
				switch ( m_tool ) {
					case TEXTURE: {
						if ( !isBrushSlopeAllowed( brushSampler, bUnrestrictedSlope, clamped_slope_range, brushGlobalPosition ) ) {
							continue;
						}
						switch ( m_operation ) {
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
										uvscale =
											scale_align[( uint8_t ) ( CLAMP( Math::round( ( fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];
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

								if ( ( base_id == asset_id_bits && fBlend < 0.5f ) ||
									( overlay_id == asset_id_bits && fBlend >= 0.5f ) ) {
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
										uvscale =
											scale_align[( uint8_t ) ( CLAMP( Math::round( ( fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];
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
								const real_t fAvg =
									average( AVG_BLEND, brushGlobalPosition, src.r, 0.f, bModifierAlt ) / 255.f;
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
							bAutoshader = ( m_operation == ADD );
							uvscale = 0.f;
							uvrotation = 0.f;
						}
						break;
					}
					case HOLES: {
						if ( fBrushAlpha > 0.5f ) {
							bHole = ( m_operation == ADD );
						}
						break;
					}
					case NAVIGATION: {
						if ( fBrushAlpha > 0.5f ) {
							bNavigation = ( m_operation == ADD );
						}
						break;
					}
					default: {
						break;
					}
				}

				// Convert back to bitfield
				const uint32_t blend_int = ( uint32_t ) ( CLAMP( Math::round( fBlend * 255.f ), 0.f, 255.f ) );
				const uint32_t bits = encBase( base_id ) | encOverlay( overlay_id ) | encBlend( blend_int ) |
								encUvRotation( uvrotation ) | encUvScale( uvscale ) | encHole( bHole ) |
								encNav( bNavigation ) | encAuto( bAutoshader );

				// Write back to pixel in FORMAT_RF. Must be a 32-bit float
				dest = Color( asFloat( bits ), 0.f, 0.f, 1.f );

			} else if ( map_type == T3DRegion::TYPE_COLOR ) {
				// Filter by visible texture
				if ( bTextureFilter ) {
					const float fSrcCtrl = asFloat( readControlBits( region_entry, map_index, mapPixelPosition ) );
					const int tex_id = ( getBlend( fSrcCtrl ) > 110 - margin ) ? getOverlay( fSrcCtrl ) : getBase( fSrcCtrl );
					if ( tex_id != asset_id ) {
						continue;
					}
				}
				if ( !isBrushSlopeAllowed( brushSampler, bUnrestrictedSlope, clamped_slope_range, brushGlobalPosition ) ) {
					continue;
				}
				switch ( m_tool ) {
					case COLOR:
						switch ( m_operation ) {
							case ADD: {
								dest = src.lerp( color, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
								dest.a = src.a;
								break;
							}
							case SUBTRACT: {
								dest = src.lerp( COLOR_WHITE, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
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
						switch ( m_operation ) {
							case ADD: {
								const real_t fTarget = .5f + .5f * roughness;
								dest.a = Math::lerp( ( real_t ) ( src.a ), fTarget, CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
								dest.a = ( float ) ( ( int ) ( dest.a * 255.f ) ) /
										 255.f; // Quantize explicitly so picked values match painted values
								break;
							}
							case SUBTRACT: {
								dest.a =
									Math::lerp( ( real_t ) ( src.a ), ( real_t ) ( .5f ), CLAMP( fBrushAlpha * strength, 0.f, 1.f ) );
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
			}
			if ( dest == src ) {
				continue;
			}
			backupRegion( region, region_entry.bMapWasImplicit );
			writeMapPixel( region_entry, map_index, mapPixelPosition, dest, map_type );
			region_entry.bModified = true;
		}
	}

	if ( bUseCompute && !compute_elements.empty() ) {
		T3DCompute::t3dBrushParams_t params;
		params.mapType = ( uint32_t ) ( map_type );
		params.tool = ( uint32_t ) ( m_tool );
		params.operation = ( uint32_t ) ( m_operation );
		params.fStrength = ( float ) ( strength );
		params.fSelectedHeight = ( float ) ( fHeight );
		params.fBrushCenterHeight = ( float ) ( rGlobalPosition.y );
		params.fRoughness = ( float ) ( roughness );
		params.colorRgba8 = packColorRgba8( color );
		params.assetId = asset_id_bits;
		params.uvRotation = compute_uv_rotation;
		params.uvScale = compute_uv_scale;
		params.featureFlags = ( bModifierAlt ? 1u : 0u ) | ( std::isfinite( rGlobalPosition.y ) ? 2u : 0u ) |
							   ( bEnableTexture ? 4u : 0u ) | ( bEnableAngle ? 8u : 0u ) | ( bEnableScale ? 16u : 0u );
		if ( !pComputeBackend->processBrush( compute_elements, params ) ) {
			LOG( T3DTerrain::WARN,
				"Compute brush failed; using buffer-equivalent CPU fallback: ", pComputeBackend->getLastError() );
			T3DCompute::processBrushCpu( compute_elements, params );
		}
		for ( size_t i = 0; i < compute_elements.size(); ++i ) {
			const T3DCompute::t3dBrushElement_t &element = compute_elements[i];
			const t3dComputeWrite_t &write = compute_writes[i];
			if ( element.value == write.sourceValue || write.iRegionIndex >= region_cache.size() ) {
				continue;
			}
			t3dRegionMapCache_t &entry = region_cache[write.iRegionIndex];
			if ( !entry.pMap || entry.pRegion.is_null() ) {
				continue;
			}
			backupRegion( entry.pRegion, entry.bMapWasImplicit );
			const size_t map_index =
				( size_t ) ( write.mapPixel.y ) * ( size_t ) ( region_size ) + ( size_t ) ( write.mapPixel.x );
			if ( map_type == T3DRegion::TYPE_HEIGHT ) {
				const float fResult = asFloat( element.value );
				writeMapPixel( entry, map_index, write.mapPixel, Color( fResult, 0.f, 0.f, 1.f ), map_type );
				entry.pRegion->updateHeight( fResult );
				pData->updateMasterHeight( fResult );
				Vector3 result_position = write.worldPosition;
				result_position.y = fResult;
				edited_area = edited_area.expand( result_position );
			} else if ( map_type == T3DRegion::TYPE_CONTROL ) {
				writeMapPixel( entry, map_index, write.mapPixel, Color( asFloat( element.value ), 0.f, 0.f, 1.f ), map_type );
			} else {
				writeMapPixel( entry, map_index, write.mapPixel, unpackColorRgba8( element.value ), map_type );
			}
			entry.bModified = true;
		}
	}

	// Publish detached row-major byte planes once per modified region. This keeps
	// the hot brush loop on raw contiguous storage while preserving Image resource
	// identity for the renderer, undo snapshots and external references.
	for ( t3dRegionMapCache_t &entry : region_cache ) {
		if ( entry.bModified && entry.bDirectAccess && entry.pMap && !entry.mapData.is_empty() ) {
			entry.pMap->set_data( region_size, region_size, entry.bMapHasMipmaps, entry.mapFormat, entry.mapData );
		}
			if ( entry.bMapWasImplicit && !entry.bModified && entry.pRegion.is_valid() &&
				( map_type == T3DRegion::TYPE_CONTROL || map_type == T3DRegion::TYPE_COLOR ) ) {
				// Region coverage is resolved before slope/texture filters are evaluated.
				// Drop transient default maps for covered regions that received no write.
				entry.pRegion->compactMapIfDefault( map_type, false );
			}
	}

	m_modifiedRegionScratch.clear();
	m_modifiedRegionScratch.reserve( region_cache.size() );
	eastl::vector<Vector2i> &modified_region_locations = m_modifiedRegionScratch;
	for ( t3dRegionMapCache_t &entry : region_cache ) {
		if ( !entry.bModified || entry.pRegion.is_null() ) {
			continue;
		}
		modified_region_locations.push_back( entry.location );
	}

	uint32_t commit_flags = T3DData::MAP_EDIT_KEEP_EDITED;
	if ( m_tool == HOLES || m_tool == HEIGHT || m_tool == SCULPT ) {
		commit_flags |= T3DData::MAP_EDIT_UPDATE_INSTANCES;
	}
	if ( m_pTerrain->getCollisionMode() == T3DCollision::DYNAMIC_EDITOR &&
		( map_type == T3DRegion::TYPE_HEIGHT || map_type == T3DRegion::TYPE_CONTROL ) ) {
		commit_flags |= T3DData::MAP_EDIT_UPDATE_COLLISION;
	}
	if ( m_tool == HEIGHT || m_tool == SCULPT || m_tool == TEXTURE || m_tool == AUTOSHADER ) {
		commit_flags |= T3DData::MAP_EDIT_SNAP;
	}
	// One typed commit owns persistence metadata, height ranges, texture-array
	// uploads, instance/collision invalidation and coalesced notifications.
	pData->commitMapEdits( map_type, modified_region_locations, edited_area, commit_flags );
}

void T3DEditor::storeUndo( void ) {
	IS_INIT_COND_MESG( !m_pTerrain->getPlugin(), "_terrain isn't initialized, returning", VOID );
	const bool bHasValidTool = m_tool >= 0 && m_tool < TOOL_MAX;
	if ( !bHasValidTool && m_originalRegions.is_empty() && m_editedRegions.is_empty() ) {
		return;
	}
	LOG( T3DTerrain::DEBUG, "Finalize undo & redo snapshots" );
	Dictionary redo_data;
	// Region locations are captured lazily only when the operation actually
	// adds or removes a region. Ordinary painting avoids copying the full active
	// location list on mouse-down.
	if ( m_undoData.has( "region_locations" ) ) {
		redo_data["region_locations"] = m_pTerrain->getData()->getRegionLocations().duplicate();
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

	if ( T3DTerrain::s_debugLevel >= T3DTerrain::DEBUG ) {
		LOG( T3DTerrain::DEBUG, "Storing Original Regions:" );
		for ( const Ref<T3DRegion> pRegion : m_originalRegions ) {
			if ( pRegion.is_valid() ) {
				pRegion->dump();
			}
		}
		LOG( T3DTerrain::DEBUG, "Storing Edited Regions:" );
		for ( const Ref<T3DRegion> pRegion : m_editedRegions ) {
			if ( pRegion.is_valid() ) {
				pRegion->dump();
			}
		}
		LOG( T3DTerrain::DEBUG, "Targeted edit snapshots: undo=", m_originalEditSnapshots.size(),
			", redo=", m_editedEditSnapshots.size() );
	}

	// Store regions that were removed or added
	if ( m_addedRemovedLocations.size() > 0 ) {
		if ( m_tool == REGION && m_operation == SUBTRACT ) {
			m_undoData["removed_regions"] = m_addedRemovedLocations;
			redo_data["added_regions"] = m_addedRemovedLocations;
			LOG( T3DTerrain::DEBUG, "Removed regions: ", m_addedRemovedLocations );
		} else {
			m_undoData["added_regions"] = m_addedRemovedLocations;
			redo_data["removed_regions"] = m_addedRemovedLocations;
			LOG( T3DTerrain::DEBUG, "Added regions: ", m_addedRemovedLocations );
		}
	}

	if ( m_pTerrain->getData()->getEditedArea().has_volume() ) {
		m_undoData["edited_area"] = m_pTerrain->getData()->getEditedArea();
		redo_data["edited_area"] = m_pTerrain->getData()->getEditedArea();
		LOG( T3DTerrain::DEBUG, "Adding edited area to snapshots: ", m_undoData["edited_area"] );
	}

	// Request the plugin store the undo/redo data.
	if ( m_pTerrain->getPlugin()->has_method( "create_undo_action" ) ) {
		LOG( T3DTerrain::INFO, "Storing undo snapshot" );
		String action_name = "Terrain3D Edit";
		if ( bHasValidTool && m_operation >= 0 && m_operation < OP_MAX ) {
			action_name = String( "Terrain3D " ) + TERRAIN3D_EDITOR_OPERATION_NAMES[m_operation] + String( " " ) + TERRAIN3D_EDITOR_TOOL_NAMES[m_tool];
		}
		LOG( T3DTerrain::DEBUG, "Creating undo action: '", action_name, "'" );
		m_pTerrain->getPlugin()->call( "create_undo_action", action_name );

		LOG( T3DTerrain::DEBUG, "Storing undo snapshot: " );
		T3DUtil::printDict( "_undo_data snapshot", m_undoData, T3DTerrain::DEBUG );
		m_pTerrain->getPlugin()->call( "add_undo_method", Callable( this, "apply_undo" ).bind( m_undoData.duplicate() ) );

		LOG( T3DTerrain::DEBUG, "Storing redo snapshot: " );
		T3DUtil::printDict( "redo_data snapshot", redo_data, T3DTerrain::DEBUG );
		m_pTerrain->getPlugin()->call( "add_do_method", Callable( this, "apply_undo" ).bind( redo_data ) );

		LOG( T3DTerrain::DEBUG, "Committing undo action" );
		m_pTerrain->getPlugin()->call( "commit_action", false );
	}
}

void T3DEditor::applyUndo( const Dictionary &rData ) {
	IS_INIT_COND_MESG( !m_pTerrain->getPlugin(), "_terrain isn't initialized, returning", VOID );
	LOG( T3DTerrain::INFO, "Applying Undo/Redo data" );

	T3DData *const pData = m_pTerrain->getData();
	eastl::vector<Vector2i> restored_region_locations;
	eastl::array<eastl::vector<Vector2i>, T3DRegion::TYPE_MAX> restored_map_locations;
	bool bInstancesChanged = false;

	if ( rData.has( "edit_snapshots" ) ) {
		const Array snapshots = rData["edit_snapshots"];
		for ( int i = 0; i < snapshots.size(); ++i ) {
			const Dictionary snapshot = snapshots[i];
			applyEditSnapshot( snapshot, restored_map_locations, bInstancesChanged );
		}
	}

	if ( rData.has( "edited_regions" ) ) {
		T3DUtil::printArr( "Edited regions", rData["edited_regions"] );
		const TypedArray<T3DRegion> undo_regions = rData["edited_regions"];
		LOG( T3DTerrain::DEBUG, "Backup has ", undo_regions.size(), " edited regions" );
		for ( Ref<T3DRegion> pRegion : undo_regions ) {
			if ( pRegion.is_null() ) {
				LOG( T3DTerrain::ERROR, "Null region saved in undo data. Please report this error." );
				continue;
			}
			pRegion->sanitizeMaps(); // Live data may not have some maps so must be sanitized
			// Replace through the repository transaction so the Dictionary, native
			// pointer map, active slot and deferred renderer reconciliation agree.
			const Error replace_error = pData->addRegion( pRegion, false );
			if ( replace_error != OK ) {
				LOG( T3DTerrain::ERROR, "Unable to restore undo region at ", pRegion->getLocation(), ": ", replace_error );
				continue;
			}
			pRegion->setModified( true ); // Tell update_maps() this region has layers that can be individually updated
			pRegion->setEdited( true );	// Flag so update_maps() will include it
			pRegion->setDeleted( false ); // Ensure region not marked for deletion
			restored_region_locations.push_back( pRegion->getLocation() );
			if ( T3DTerrain::s_debugLevel >= T3DTerrain::DEBUG ) {
				LOG( T3DTerrain::DEBUG, "Restoring region:" );
				pRegion->dump();
			}
		}
	}

	if ( rData.has( "edited_area" ) ) {
		LOG( T3DTerrain::DEBUG, "Edited area: ", rData["edited_area"] );
		pData->addEditedArea( rData["edited_area"] );
	}

	if ( rData.has( "added_regions" ) ) {
		LOG( T3DTerrain::DEBUG, "Added regions: ", rData["added_regions"] );
		const TypedArray<Vector2i> region_locs = rData["added_regions"];
		for ( const Vector2i region_loc : region_locs ) {
			Ref<T3DRegion> pRegion = pData->getRegion( region_loc );
			if ( pRegion.is_valid() ) {
				LOG( T3DTerrain::DEBUG, "Marking region: ", region_loc, " +deleted, +modified, ", ptrToStr( *pRegion ) );
				pRegion->setDeleted( true );
				pRegion->setModified( true );
			}
		}
	}
	if ( rData.has( "removed_regions" ) ) {
		LOG( T3DTerrain::DEBUG, "Removed regions: ", rData["removed_regions"] );
		const TypedArray<Vector2i> region_locs = rData["removed_regions"];
		for ( const Vector2i region_loc : region_locs ) {
			Ref<T3DRegion> pRegion = pData->getRegion( region_loc );
			if ( pRegion.is_valid() ) {
				LOG( T3DTerrain::DEBUG, "Marking region: ", region_loc, " -deleted, +modified, ", ptrToStr( *pRegion ) );
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
		const TypedArray<Vector2i> locations = rData["region_locations"];
		m_pTerrain->getData()->setRegionLocations( locations.duplicate() );
		bRestoredRegionLayout = true;
		LOG( T3DTerrain::DEBUG, "Locations(", locations.size(), "): ", locations );
	}
	// Legacy/external undo payloads may omit region_locations. Only those require
	// the old full rebuild fallback.
	if ( ( rData.has( "added_regions" ) || rData.has( "removed_regions" ) ) && !bRestoredRegionLayout ) {
		pData->updateMaps( T3DRegion::TYPE_MAX, true, false );
	}
	const AABB restored_area = rData.has( "edited_area" ) ? AABB( rData["edited_area"] ) : AABB();
	for ( int map_type = T3DRegion::TYPE_HEIGHT; map_type < T3DRegion::TYPE_MAX; ++map_type ) {
		eastl::vector<Vector2i> commit_locations = restored_map_locations[( size_t ) ( map_type )];
		commit_locations.insert( commit_locations.end(), restored_region_locations.begin(),
								restored_region_locations.end() );
		std::sort( commit_locations.begin(), commit_locations.end(), locationLess );
		commit_locations.erase( std::unique( commit_locations.begin(), commit_locations.end() ), commit_locations.end() );
		if ( commit_locations.empty() ) {
			continue;
		}
		uint32_t commit_flags = 0u;
		if ( map_type == T3DRegion::TYPE_HEIGHT || map_type == T3DRegion::TYPE_CONTROL ) {
			commit_flags |= T3DData::MAP_EDIT_UPDATE_INSTANCES | T3DData::MAP_EDIT_UPDATE_COLLISION |
							T3DData::MAP_EDIT_SNAP;
		}
		pData->commitMapEdits( ( t3dMapType_t ) ( map_type ), commit_locations, restored_area, commit_flags );
	}
	// After TextureArray updates clear edited regions flag.
	if ( rData.has( "edited_regions" ) ) {
		const TypedArray<T3DRegion> undo_regions = rData["edited_regions"];
		for ( Ref<T3DRegion> pRegion : undo_regions ) {
			if ( pRegion.is_valid() ) {
				pRegion->setEdited( false );
			}
		}
	}
	if ( bInstancesChanged || rData.has( "edited_regions" ) || rData.has( "added_regions" ) ||
		rData.has( "removed_regions" ) ) {
		m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, true );
	}
}

// Returns average of height, blend (as real_t(0-255)), or roughness. Overloaded version handles average color
float T3DEditor::average(
	const t3dAverageMode_t mode, const Vector3 &rGlobalPosition, const float fBase,
	const float fNanVal, const bool bAlt
) const {
	IS_DATA_INIT( NAN );
	T3DData *const pData = m_pTerrain->getData();
	const real_t fVertexSpacing = m_pTerrain->getVertexSpacing();
	const Vector3 left_position = rGlobalPosition - Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3 right_position = rGlobalPosition + Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3 down_position = rGlobalPosition - Vector3( 0.f, 0.f, fVertexSpacing );
	const Vector3 up_position = rGlobalPosition + Vector3( 0.f, 0.f, fVertexSpacing );

	t3dMapType_t map_type;
	int index;
	switch ( mode ) {
		case AVG_HEIGHT:
			map_type = T3DRegion::TYPE_HEIGHT;
			index = 0; // Red
			break;
		case AVG_BLEND:
			map_type = T3DRegion::TYPE_CONTROL;
			index = 0; // Red
			break;
		case AVG_ROUGHNESS:
			map_type = T3DRegion::TYPE_COLOR;
			index = 3; // Alpha
			break;
		default:
			break;
	}

	Color pixel;
	float fLeft, right, up, down;
	pixel = pData->getPixel( map_type, left_position );
	fLeft = std::isnan( pixel.r ) ? fNanVal : pixel[index];
	pixel = pData->getPixel( map_type, right_position );
	right = std::isnan( pixel.r ) ? fNanVal : pixel[index];
	pixel = pData->getPixel( map_type, up_position );
	up = std::isnan( pixel.r ) ? fNanVal : pixel[index];
	pixel = pData->getPixel( map_type, down_position );
	down = std::isnan( pixel.r ) ? fNanVal : pixel[index];

	if ( mode == AVG_BLEND ) {
		if ( bAlt ) {
			return ( getBlend( fBase ) + getBlend( fLeft ) + getBlend( right ) + getBlend( up ) + getBlend( down ) ) * 0.2f;
		} else {
			return Math::lerp( ( float ) ( getBlend( fBase ) ), 128.f, .1f );
		}
	} else {
		return ( fBase + fLeft + right + up + down ) * 0.2f;
	}
}

Color T3DEditor::average( const Vector3 &rGlobalPosition, const Color &clrBase ) const {
	IS_DATA_INIT( COLOR_NAN );
	T3DData *const pData = m_pTerrain->getData();
	const real_t fVertexSpacing = m_pTerrain->getVertexSpacing();
	const Vector3 left_position = rGlobalPosition - Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3 right_position = rGlobalPosition + Vector3( fVertexSpacing, 0.f, 0.f );
	const Vector3 down_position = rGlobalPosition - Vector3( 0.f, 0.f, fVertexSpacing );
	const Vector3 up_position = rGlobalPosition + Vector3( 0.f, 0.f, fVertexSpacing );

	Color left = pData->getPixel( T3DRegion::TYPE_COLOR, left_position ).srgb_to_linear();
	if ( std::isnan( left.r ) ) {
		left = COLOR_WHITE;
	}
	Color right = pData->getPixel( T3DRegion::TYPE_COLOR, right_position ).srgb_to_linear();
	if ( std::isnan( right.r ) ) {
		right = COLOR_WHITE;
	}
	Color up = pData->getPixel( T3DRegion::TYPE_COLOR, up_position ).srgb_to_linear();
	if ( std::isnan( up.r ) ) {
		up = COLOR_WHITE;
	}
	Color down = pData->getPixel( T3DRegion::TYPE_COLOR, down_position ).srgb_to_linear();
	if ( std::isnan( down.r ) ) {
		down = COLOR_WHITE;
	}
	Color base = clrBase.srgb_to_linear();
	return Color( ( base.r + left.r + right.r + up.r + down.r ) * 0.2f, ( base.g + left.g + right.g + up.g + down.g ) * 0.2f,
				 ( base.b + left.b + right.b + up.b + down.b ) * 0.2f, 1.f )
		.linear_to_srgb();
}

///////////////////////////
// Public Functions
///////////////////////////

// Santize and set incoming brush data w/ defaults and clamps
// Only santizes data needed for the editor, other parameters (eg instancer) untouched here
void T3DEditor::setBrushData( const Dictionary &rData ) {
	m_brushData = rData; // Same instance. Anything could be inserted after this, eg mouse_pressure
	m_pCachedBrushImage = nullptr;
	m_cachedBrushImageSize = V2I_ZERO;
	m_fCachedBrushGamma = -1.f;
	m_fBrushAlphaCache.clear();

	// Sanitize image and textures
	Array brush_images = rData["brush"];
	if ( brush_images.size() == 2 ) {
		const Ref<Image> pImg = brush_images[0];
		if ( pImg.is_valid() && !pImg->is_empty() ) {
			m_brushData["brush_image"] = pImg;
			m_brushData["brush_image_size"] = pImg->get_size();
		} else {
			LOG( T3DTerrain::ERROR, "Brush data doesn't contain a valid image" );
		}
		const Ref<Texture2D> pTex = brush_images[1];
		if ( pTex.is_valid() && pTex->get_width() > 0 && pTex->get_height() > 0 ) {
			m_brushData["brush_texture"] = pTex;
		} else {
			LOG( T3DTerrain::ERROR, "Brush data doesn't contain a valid texture" );
		}
	} else {
		LOG( T3DTerrain::ERROR, "Brush data doesn't contain an image and texture" );
	}

	// Santize settings
	// size is redundantly clamped differently in _operate_map and instancer::add_transforms
	m_brushData["size"] = CLAMP( ( real_t ) ( rData.get( "size", 10.f ) ), 0.1f, 4096.f ); // Diameter in meters
	m_brushData["strength"] =
		CLAMP( ( real_t ) ( rData.get( "strength", .1f ) ) * .01f, .01f, 1000.f ); // 1-100k% (max of 1000m per click)
	// mouse_pressure injected in editor.gd and sanitized in _operate_map()
	Vector2 slope = rData.get( "slope", Vector2( 0.f, 90.f ) );
	slope.x = CLAMP( slope.x, 0.f, 90.f );
	slope.y = CLAMP( slope.y, 0.f, 90.f );
	m_brushData["slope"] = slope;														 // 0-90 (degrees)
	m_brushData["height"] = CLAMP( ( real_t ) ( rData.get( "height", 0.f ) ), -65536.f, 65536.f ); // Meters
	Color col = rData.get( "color", COLOR_ROUGHNESS );
	col.r = CLAMP( col.r, 0.f, 5.f );
	col.g = CLAMP( col.g, 0.f, 5.f );
	col.b = CLAMP( col.b, 0.f, 5.f );
	col.a = CLAMP( col.a, 0.f, 1.f );
	m_brushData["color"] = col;
	m_brushData["roughness"] = CLAMP( ( real_t ) ( rData.get( "roughness", 0.f ) ), -100.f, 100.f ) * .01f; // Percentage

	m_brushData["enable_texture"] = rData.get( "enable_texture", true );
	m_brushData["texture_filter"] = rData.get( "texture_filter", false );
	const int requested_asset_id = ( int ) ( rData.get( "asset_id", -1 ) );
	const int maximum_asset_id = MAX( TERRAIN3D_MAX_MESH_ASSETS, TERRAIN3D_MAX_TEXTURE_ASSETS ) - 1;
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

	T3DUtil::printDict( "set_brush_data() Santized brush data:", m_brushData, T3DTerrain::EXTREME );
}

void T3DEditor::setTool( const t3dTool_t tool ) {
	const t3dTool_t new_tool = CLAMP( tool, ( t3dTool_t ) ( 0 ), TOOL_MAX );
	if ( new_tool == m_tool ) {
		return;
	}
	const t3dTool_t old_tool = m_tool;
	m_tool = new_tool;
	if ( m_pTerrain && ( m_tool == t3dTool_t::NAVIGATION || old_tool == t3dTool_t::NAVIGATION || m_tool == t3dTool_t::REGION ||
					 old_tool == t3dTool_t::REGION ) ) {
		m_pTerrain->getMaterial()->update( T3DMaterial::FULL_REBUILD );
	}
}

void T3DEditor::setOperation( const t3dOperation_t operation ) {
	SET_IF_DIFF( m_operation, CLAMP( operation, ( t3dOperation_t ) ( 0 ), OP_MAX ) );
}

// Called on mouse click
void T3DEditor::startOperation( const Vector3 &rGlobalPosition ) {
	IS_DATA_INIT_MESG( "Terrain isn't initialized", VOID );
	LOG( T3DTerrain::INFO, "t3dSetting_t up undo snapshot" );
	m_undoData.clear();
	m_bIsOperating = true;
	m_originalRegions = TypedArray<T3DRegion>(); // New pointers instead of clear
	m_editedRegions = TypedArray<T3DRegion>();
	m_originalEditSnapshots = Array();
	m_editedEditSnapshots = Array();
	m_addedRemovedLocations = TypedArray<Vector2i>();
	// Reset counter at start to ensure first click places an instance
	m_pTerrain->getInstancer()->resetDensityCounter();
	m_pTerrain->getData()->clearEditedArea();
	m_operationPosition = rGlobalPosition;
	m_operationMovement = V3_ZERO;
}

// Called on mouse movement with left mouse button down
void T3DEditor::operate( const Vector3 &rGlobalPosition, const real_t fCameraDirection ) {
	IS_DATA_INIT_MESG( "Terrain isn't initialized", VOID );
	if ( !m_bIsOperating ) {
		LOG( T3DTerrain::ERROR, "Run start_operation() before operating" );
		return;
	}
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
		operateRegion( m_pTerrain->getData()->getRegionLocation( rGlobalPosition ) );
	} else if ( m_tool >= 0 && m_tool < TOOL_MAX ) {
		operateMap( rGlobalPosition, fCameraDirection );
	}
}

void T3DEditor::backupRegion( const Ref<T3DRegion> &rRegion, const bool bMapWasImplicit ) {
	// Backup region once at the start of an operation. Once Edited is set, this is skipped
	if ( m_bIsOperating && rRegion.is_valid() && !rRegion->isEdited() ) {
		LOG( T3DTerrain::DEBUG, "Storing original copy of region: ", rRegion->getLocation() );
		// A newly auto-created region is already represented by the add/remove
		// snapshot and the redo region. Capturing another map snapshot is redundant.
		if ( m_addedRemovedLocations.find( rRegion->getLocation() ) < 0 ) {
			if ( m_tool == REGION ) {
				const Ref<T3DRegion> pOrigRegion = rRegion->duplicate( true );
				m_originalRegions.push_back( pOrigRegion );
				m_editedRegions.push_back( rRegion );
			} else {
				const Dictionary snapshot = captureEditSnapshot( rRegion, Dictionary(), bMapWasImplicit );
				if ( !snapshot.is_empty() ) {
					m_originalEditSnapshots.push_back( snapshot );
				}
			}
		}
		rRegion->setEdited( true );
		rRegion->setModified( true );
		if ( T3DTerrain::s_debugLevel >= T3DTerrain::DEBUG ) {
			LOG( T3DTerrain::DEBUG, "Backed up region edit state" );
			rRegion->dump();
		}
	}
}

void T3DEditor::backupRegionFull( const Ref<T3DRegion> &rRegion ) {
	// External editor jobs such as erosion can update height, control and color in
	// one transaction. Preserve the complete region independently of the active
	// brush tool so undo restores every channel changed by the job.
	if ( !m_bIsOperating || rRegion.is_null() || rRegion->isEdited() ) {
		return;
	}

	LOG( T3DTerrain::DEBUG, "Storing full original copy of region: ", rRegion->getLocation() );
	if ( m_addedRemovedLocations.find( rRegion->getLocation() ) < 0 ) {
		const Ref<T3DRegion> pOrigRegion = rRegion->duplicate( true );
		m_originalRegions.push_back( pOrigRegion );
		m_editedRegions.push_back( rRegion );
	}
	rRegion->setEdited( true );
	rRegion->setModified( true );
}

void T3DEditor::trimBrushScratch( void ) {
	m_xAxisScratch.clear();
	m_zAxisScratch.clear();
	m_regionCacheScratch.clear();
	m_computeElementScratch.clear();
	m_computeWriteScratch.clear();
	m_modifiedRegionScratch.clear();

	const size_t compute_bytes =
		m_computeElementScratch.capacity() * sizeof( T3DCompute::t3dBrushElement_t ) +
		m_computeWriteScratch.capacity() * sizeof( t3dComputeWrite_t );
	if ( compute_bytes > TERRAIN3D_COMPUTE_SCRATCH_RETAIN_LIMIT ) {
		eastl::vector<T3DCompute::t3dBrushElement_t>().swap( m_computeElementScratch );
		eastl::vector<t3dComputeWrite_t>().swap( m_computeWriteScratch );
	}

	// Retain no more region-cache entries than the active logical terrain can use.
	int region_map_size = TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
	if ( m_pTerrain && m_pTerrain->getData() ) {
		region_map_size = m_pTerrain->getData()->getRegionMapSizeInternal();
	}
	const size_t region_cache_limit = ( size_t ) ( region_map_size ) * ( size_t ) ( region_map_size );
	if ( m_regionCacheScratch.capacity() > region_cache_limit ) {
		eastl::vector<t3dRegionMapCache_t>().swap( m_regionCacheScratch );
	}
}

// Called on left mouse button released
void T3DEditor::stopOperation( void ) {
	IS_DATA_INIT_MESG( "Terrain isn't initialized", VOID );
	// If undo was created and terrain actually modified, store it
	LOG( T3DTerrain::DEBUG, "Backed up regions: ", m_originalRegions.size(), ", Edited regions: ", m_editedRegions.size(),
		", Targeted snapshots: ", m_originalEditSnapshots.size(),
		", Added/Removed regions: ", m_addedRemovedLocations.size() );
	if ( m_bIsOperating &&
		( !m_addedRemovedLocations.is_empty() || !m_editedRegions.is_empty() || !m_originalEditSnapshots.is_empty() ) ) {
		for ( int i = 0; i < m_editedRegions.size(); i++ ) {
			Ref<T3DRegion> pRegion = m_editedRegions[i];
			pRegion->setEdited( false );
			// Make duplicate for redo, necessary or redos won't work
			const Ref<T3DRegion> pRedoRegion = pRegion->duplicate( true );
			m_editedRegions[i] = pRedoRegion;
			if ( T3DTerrain::s_debugLevel >= T3DTerrain::DEBUG ) {
				LOG( T3DTerrain::DEBUG, "Edited region:" );
				pRegion->dump();
				LOG( T3DTerrain::DEBUG, "Redo region:" );
				pRedoRegion->dump();
			}
		}
		for ( int i = 0; i < m_originalEditSnapshots.size(); ++i ) {
			const Dictionary original_snapshot = m_originalEditSnapshots[i];
			if ( !original_snapshot.has( "location" ) ) {
				continue;
			}
			const Vector2i location = original_snapshot["location"];
			Ref<T3DRegion> pRegion = m_pTerrain->getData()->getRegion( location );
			if ( pRegion.is_valid() ) {
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
	m_originalRegions = TypedArray<T3DRegion>(); // New pointers instead of clear
	m_editedRegions = TypedArray<T3DRegion>();
	m_originalEditSnapshots = Array();
	m_editedEditSnapshots = Array();
	m_addedRemovedLocations = TypedArray<Vector2i>();
	m_pTerrain->getData()->clearEditedArea();
	m_bIsOperating = false;
	trimBrushScratch();
}


///////////////////////////
// Protected Functions
///////////////////////////

void T3DEditor::_bind_methods( void ) {
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

	ClassDB::bind_method( D_METHOD( "set_terrain", "terrain" ), &T3DEditor::setTerrain );
	ClassDB::bind_method( D_METHOD( "get_terrain" ), &T3DEditor::getTerrain );

	ClassDB::bind_method( D_METHOD( "set_brush_data", "data" ), &T3DEditor::setBrushData );
	ClassDB::bind_method( D_METHOD( "set_tool", "tool" ), &T3DEditor::setTool );
	ClassDB::bind_method( D_METHOD( "get_tool" ), &T3DEditor::getTool );
	ClassDB::bind_method( D_METHOD( "set_operation", "operation" ), &T3DEditor::setOperation );
	ClassDB::bind_method( D_METHOD( "get_operation" ), &T3DEditor::getOperation );
	ClassDB::bind_method( D_METHOD( "start_operation", "position" ), &T3DEditor::startOperation );
	ClassDB::bind_method( D_METHOD( "is_operating" ), &T3DEditor::isOperating );
	ClassDB::bind_method( D_METHOD( "operate", "position", "camera_direction" ), &T3DEditor::operate );
	ClassDB::bind_method( D_METHOD( "backup_region", "region", "map_was_implicit" ), &T3DEditor::backupRegion, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "backup_region_full", "region" ), &T3DEditor::backupRegionFull );
	ClassDB::bind_method( D_METHOD( "stop_operation" ), &T3DEditor::stopOperation );

	ClassDB::bind_method( D_METHOD( "apply_undo", "data" ), &T3DEditor::applyUndo );
}
