// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/terrain_3d_landform_baker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include "eastl_config.h"
#include <EASTL/vector.h>

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "Terrain3DConstants.h"
#include "core/logger.h"
#include "terrain_3d.h"
#include "assets/terrain_3d_assets.h"
#include "physics/terrain_3d_collision.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "simd/terrain_3d_parallel.h"
#include "data/terrain_3d_data.h"
#include "authoring/terrain_3d_editor.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_sampler.h"
#include "assets/terrain_3d_texture_asset.h"
#include "core/terrain_3d_util.h"

namespace {
typedef struct t3dsettings_s {
	Terrain3DLandformBaker::t3dmode_t mode = Terrain3DLandformBaker::RIDGE;
	Terrain3DLandformBaker::t3dmorphologyMode_t morphology_mode = Terrain3DLandformBaker::MORPH_EXPAND_PEAKS;
	float fWidth = 32.0f;
	float fAmplitude = 12.0f;
	float fStrength = 1.0f;
	float fFalloff = 0.65f;
	float fCrestWidth = 0.18f;
	float fAsymmetry = 0.0f;
	float fCliffSide = 1.0f;
	float noise = 0.12f;
	float noise_scale = 24.0f;
	float fTerraceStep = 4.0f;
	float fRoadCrown = 0.15f;
	float water_level = 0.0f;
	float fShoreSlope = 0.08f;
	float fBasinFloorWidth = 0.35f;
	bool bBasinCreateRegions = false;
	float fDuneSpacing = 28.0f;
	float fAngleOfRepose = 36.0f;
	int iterations = 4;
	int seed = 1337;
	bool bAutoRegions = true;
	bool bPaintMaterial = false;
	int base_texture = 0;
	int overlay_texture = 0;
	float fMaterialBlend = 0.0f;
	bool bGenerateMesh = false;
	bool bGenerateCollision = true;
	Terrain3DLandformBaker::t3dgeometryMode_t geometry_mode = Terrain3DLandformBaker::GEOMETRY_PROCEDURAL_UNIQUE;
	float fMeshDepth = 5.0f;
	float fMeshIrregularity = 0.28f;
	float fMeshSampleSpacing = 3.0f;
	float fMeshLip = 1.25f;
	float fMeshToe = 1.5f;
	float fModuleLength = 8.0f;
	float fModuleCornerAngle = 18.0f;
	float fModuleMinScale = 0.65f;
	float fModuleMaxScale = 1.75f;
	float fCollisionSpacing = 8.0f;
	eastl::vector<int> module_mesh_ids;
	eastl::vector<int> corner_mesh_ids;
	eastl::vector<int> end_mesh_ids;
} t3dsettings_t;

typedef struct t3dpathProjection_s {
	float distance = std::numeric_limits<float>::infinity();
	float fSignedDistance = 0.0f;
	float fPathDistance = 0.0f;
	float fPathHeight = 0.0f;
	Vector2 tangent = Vector2( 1.0f, 0.0f );
	Vector2 position = V2_ZERO;
} t3dpathProjection_t;

typedef struct t3dbasinProfile_s {
	float fPathLength = 0.0f;
	float fHeightSlope = 0.0f;
	float fHeightIntercept = 0.0f;
	bool bValid = false;
} t3dbasinProfile_t;

typedef struct t3dregionEdit_s {
	Vector2i location = V2I_ZERO;
	Ref<Terrain3DRegion> pRegion;
	Ref<Image> height;
	Ref<Image> pControl;
	eastl::vector<uint8_t> height_snapshot;
	Image::Format height_format = Image::FORMAT_MAX;
	eastl::vector<float> source_values;
	eastl::vector<float> output_values;
	bool bCalculationValid = true;
	bool bExistedBefore = true;
	bool bHeightModified = false;
	bool bControlModified = false;
} t3dregionEdit_t;

typedef struct t3dlandformModuleCandidate_s {
	int meshId = -1;
	float fScore = std::numeric_limits<float>::max();
} t3dlandformModuleCandidate_t;

static bool landformModuleCandidateLess( const t3dlandformModuleCandidate_t &rA, const t3dlandformModuleCandidate_t &rB ) {
	return rA.fScore < rB.fScore;
}

static float sampleHeightOr( T3DSampler &rSampler, const Vector2 &rPosition, const float fFallback ) {
	const float fSampled = rSampler.sampleHeight( Vector3( rPosition.x, 0.0f, rPosition.y ) );
	return std::isfinite( fSampled ) ? fSampled : fFallback;
}

static eastl::vector<int> parseMeshIds( const Variant &rValue ) {
	eastl::vector<int> result;
	if ( rValue.get_type() == Variant::PACKED_INT32_ARRAY ) {
		const PackedInt32Array values = rValue;
		result.reserve( ( size_t ) ( values.size() ) );
		for ( int index = 0; index < values.size(); ++index ) {
			if ( values[index] >= 0 ) {
				result.push_back( values[index] );
			}
		}
	} else if ( rValue.get_type() == Variant::ARRAY ) {
		const Array values = rValue;
		result.reserve( ( size_t ) ( values.size() ) );
		for ( int index = 0; index < values.size(); ++index ) {
			const int value = ( int ) ( values[index] );
			if ( value >= 0 ) {
				result.push_back( value );
			}
		}
	}
	return result;
}

static t3dsettings_t parseSettings( const Dictionary &rDictionary ) {
	t3dsettings_t settings;
	settings.mode = ( Terrain3DLandformBaker::t3dmode_t ) ( CLAMP( ( int ) ( rDictionary.get( "mode", ( int ) ( settings.mode ) ) ), 0,
														 ( int ) ( Terrain3DLandformBaker::MODE_MAX ) - 1 ) );
	settings.morphology_mode = ( Terrain3DLandformBaker::t3dmorphologyMode_t ) ( CLAMP( ( int ) ( rDictionary.get( "morphology_mode", ( int ) ( settings.morphology_mode ) ) ), 0,
			  ( int ) ( Terrain3DLandformBaker::MORPH_MAX ) - 1 ) );
	settings.fWidth = MAX( ( float ) ( rDictionary.get( "width", settings.fWidth ) ), 1.0f );
	settings.fAmplitude = MAX( ( float ) ( rDictionary.get( "amplitude", settings.fAmplitude ) ), 0.0f );
	settings.fStrength = CLAMP( ( float ) ( rDictionary.get( "strength", settings.fStrength ) ), 0.0f, 1.0f );
	settings.fFalloff = CLAMP( ( float ) ( rDictionary.get( "falloff", settings.fFalloff ) ), 0.01f, 1.0f );
	settings.fCrestWidth = CLAMP( ( float ) ( rDictionary.get( "crest_width", settings.fCrestWidth ) ), 0.0f, 0.95f );
	settings.fAsymmetry = CLAMP( ( float ) ( rDictionary.get( "asymmetry", settings.fAsymmetry ) ), -1.0f, 1.0f );
	settings.fCliffSide = ( int ) ( rDictionary.get( "cliff_side", 1 ) ) == 0 ? -1.0f : 1.0f;
	settings.noise = CLAMP( ( float ) ( rDictionary.get( "noise", settings.noise ) ), 0.0f, 1.0f );
	settings.noise_scale = MAX( ( float ) ( rDictionary.get( "noise_scale", settings.noise_scale ) ), 0.25f );
	settings.fTerraceStep = MAX( ( float ) ( rDictionary.get( "terrace_step", settings.fTerraceStep ) ), 0.05f );
	settings.fRoadCrown = ( float ) ( rDictionary.get( "road_crown", settings.fRoadCrown ) );
	settings.water_level = ( float ) ( rDictionary.get( "water_level", settings.water_level ) );
	settings.fShoreSlope = ( float ) ( rDictionary.get( "shore_slope", settings.fShoreSlope ) );
	settings.fBasinFloorWidth =
		CLAMP( ( float ) ( rDictionary.get( "basin_floor_width", settings.fBasinFloorWidth ) ), 0.0f, 0.95f );
	settings.bBasinCreateRegions = ( bool ) ( rDictionary.get( "basin_create_regions", settings.bBasinCreateRegions ) );
	settings.fDuneSpacing = MAX( ( float ) ( rDictionary.get( "dune_spacing", settings.fDuneSpacing ) ), 1.0f );
	settings.fAngleOfRepose = CLAMP( ( float ) ( rDictionary.get( "angle_of_repose", settings.fAngleOfRepose ) ), 1.0f, 89.0f );
	settings.iterations = CLAMP( ( int ) ( rDictionary.get( "iterations", settings.iterations ) ), 1, 32 );
	settings.seed = ( int ) ( rDictionary.get( "seed", settings.seed ) );
	settings.bAutoRegions = ( bool ) ( rDictionary.get( "auto_regions", settings.bAutoRegions ) );
	settings.bPaintMaterial = ( bool ) ( rDictionary.get( "paint_material", settings.bPaintMaterial ) );
	settings.base_texture = CLAMP( ( int ) ( rDictionary.get( "base_texture", settings.base_texture ) ), 0, 31 );
	settings.overlay_texture = CLAMP( ( int ) ( rDictionary.get( "overlay_texture", settings.overlay_texture ) ), 0, 31 );
	settings.fMaterialBlend = CLAMP( ( float ) ( rDictionary.get( "material_blend", settings.fMaterialBlend ) ), 0.0f, 1.0f );
	settings.bGenerateMesh = ( bool ) ( rDictionary.get( "generate_mesh", settings.bGenerateMesh ) );
	settings.bGenerateCollision = ( bool ) ( rDictionary.get( "generate_collision", settings.bGenerateCollision ) );
	settings.geometry_mode = ( Terrain3DLandformBaker::t3dgeometryMode_t ) ( CLAMP(
		( int ) ( rDictionary.get( "geometry_mode", ( int ) ( settings.geometry_mode ) ) ), 0,
		( int ) ( Terrain3DLandformBaker::GEOMETRY_MAX ) - 1 ) );
	settings.fMeshDepth = MAX( ( float ) ( rDictionary.get( "mesh_depth", settings.fMeshDepth ) ), 0.05f );
	settings.fMeshIrregularity = CLAMP( ( float ) ( rDictionary.get( "mesh_irregularity", settings.fMeshIrregularity ) ), 0.0f, 1.0f );
	settings.fMeshSampleSpacing = MAX( ( float ) ( rDictionary.get( "mesh_sample_spacing", settings.fMeshSampleSpacing ) ), 0.25f );
	settings.fMeshLip = MAX( ( float ) ( rDictionary.get( "mesh_lip", settings.fMeshLip ) ), 0.0f );
	settings.fMeshToe = MAX( ( float ) ( rDictionary.get( "mesh_toe", settings.fMeshToe ) ), 0.0f );
	settings.fModuleLength = MAX( ( float ) ( rDictionary.get( "module_length", settings.fModuleLength ) ), 0.5f );
	settings.fModuleCornerAngle =
		CLAMP( ( float ) ( rDictionary.get( "module_corner_angle", settings.fModuleCornerAngle ) ), 0.0f, 180.0f );
	settings.fModuleMinScale =
		CLAMP( ( float ) ( rDictionary.get( "module_min_scale", settings.fModuleMinScale ) ), 0.05f, 10.0f );
	settings.fModuleMaxScale = MAX(
		( float ) ( rDictionary.get( "module_max_scale", settings.fModuleMaxScale ) ), settings.fModuleMinScale );
	settings.fCollisionSpacing = MAX(
		( float ) ( rDictionary.get( "collision_spacing", settings.fCollisionSpacing ) ), 0.5f );
	settings.module_mesh_ids = parseMeshIds( rDictionary.get( "module_mesh_ids", PackedInt32Array() ) );
	settings.corner_mesh_ids = parseMeshIds( rDictionary.get( "corner_mesh_ids", PackedInt32Array() ) );
	settings.end_mesh_ids = parseMeshIds( rDictionary.get( "end_mesh_ids", PackedInt32Array() ) );
	return settings;
}

static uint32_t hashU32( uint32_t nValue ) {
	nValue ^= nValue >> 16u;
	nValue *= 0x7feb352du;
	nValue ^= nValue >> 15u;
	nValue *= 0x846ca68bu;
	nValue ^= nValue >> 16u;
	return nValue;
}

static float signedNoise( const int iX, const int iZ, const int nSeed ) {
	const uint32_t x = ( uint32_t ) ( iX ) * TERRAIN3D_HASH_GOLDEN_RATIO_32;
	const uint32_t z = ( uint32_t ) ( iZ ) * TERRAIN3D_HASH_MIX_32;
	const uint32_t value = hashU32( x ^ z ^ ( uint32_t ) ( nSeed ) );
	return ( float ) ( value & 0x00ffffffu ) / ( float ) ( 0x007fffffu ) - 1.0f;
}

static float smooth01( const float fValue ) {
	const float value = CLAMP( fValue, 0.0f, 1.0f );
	return value * value * ( 3.0f - 2.0f * value );
}

static float profileWeight(
	const float fDistance, const float fHalfWidth, const float fCrestWidth,
	const float fFalloff
) {
	if ( fHalfWidth <= TERRAIN3D_LANDFORM_EPSILON || fDistance >= fHalfWidth ) {
		return 0.0f;
	}
	const float fNormalized = fDistance / fHalfWidth;
	if ( fNormalized <= fCrestWidth ) {
		return 1.0f;
	}
	const float fDenominator = MAX( 1.0f - fCrestWidth, TERRAIN3D_LANDFORM_EPSILON );
	float fEdge = 1.0f - ( fNormalized - fCrestWidth ) / fDenominator;
	fEdge = Math::pow( CLAMP( fEdge, 0.0f, 1.0f ), Math::lerp( 3.5f, 0.55f, fFalloff ) );
	return smooth01( fEdge );
}

static float segmentLengthXz( const Vector3 &vA, const Vector3 &rB ) {
	return Vector2( rB.x - vA.x, rB.z - vA.z ).length();
}

static t3dpathProjection_t projectToPath( const PackedVector3Array &arrPath, const Vector2 &rPoint ) {
	t3dpathProjection_t best;
	float fAccumulated = 0.0f;
	for ( int index = 0; index + 1 < arrPath.size(); ++index ) {
		const Vector3 a3 = arrPath[index];
		const Vector3 b3 = arrPath[index + 1];
		const Vector2 a( a3.x, a3.z );
		const Vector2 b( b3.x, b3.z );
		const Vector2 segment = b - a;
		const float length_squared = segment.length_squared();
		if ( length_squared <= TERRAIN3D_LANDFORM_EPSILON ) {
			continue;
		}
		const float fT = CLAMP( ( rPoint - a ).dot( segment ) / length_squared, 0.0f, 1.0f );
		const Vector2 position = a + segment * fT;
		const Vector2 delta = rPoint - position;
		const float fDistance = delta.length();
		if ( fDistance < best.distance ) {
			const float length = Math::sqrt( length_squared );
			const Vector2 tangent = segment / length;
			best.distance = fDistance;
			best.fSignedDistance = tangent.cross( delta );
			best.fPathDistance = fAccumulated + length * fT;
			best.fPathHeight = Math::lerp( a3.y, b3.y, fT );
			best.tangent = tangent;
			best.position = position;
		}
		fAccumulated += Math::sqrt( length_squared );
	}
	if ( !std::isfinite( best.distance ) && !arrPath.is_empty() ) {
		const Vector3 point = arrPath[0];
		best.position = Vector2( point.x, point.z );
		best.distance = rPoint.distance_to( best.position );
		best.fPathHeight = point.y;
	}
	return best;
}

static t3dbasinProfile_t buildBasinProfile( const PackedVector3Array &arrPath ) {
	t3dbasinProfile_t profile;
	if ( arrPath.is_empty() ) {
		return profile;
	}

	double fCumulative = 0.0;
	double fSumDistance = 0.0;
	double fSumHeight = 0.0;
	double fSumDistanceSquared = 0.0;
	double fSumDistanceHeight = 0.0;
	int sample_count = 0;
	for ( int index = 0; index < arrPath.size(); ++index ) {
		if ( index > 0 ) {
			fCumulative += ( double ) ( segmentLengthXz( arrPath[index - 1], arrPath[index] ) );
		}
		const double fHeight = ( double ) ( arrPath[index].y );
		if ( !std::isfinite( fCumulative ) || !std::isfinite( fHeight ) ) {
			continue;
		}
		fSumDistance += fCumulative;
		fSumHeight += fHeight;
		fSumDistanceSquared += fCumulative * fCumulative;
		fSumDistanceHeight += fCumulative * fHeight;
		++sample_count;
	}
	if ( sample_count <= 0 ) {
		return profile;
	}

	const double fDenominator = ( double ) ( sample_count ) * fSumDistanceSquared - fSumDistance * fSumDistance;
	double fSlope = 0.0;
	if ( Math::abs( fDenominator ) > ( double ) ( TERRAIN3D_LANDFORM_EPSILON ) ) {
		fSlope = ( ( double ) ( sample_count ) * fSumDistanceHeight - fSumDistance * fSumHeight ) / fDenominator;
	}
	const double fIntercept = ( fSumHeight - fSlope * fSumDistance ) / ( double ) ( sample_count );
	profile.fPathLength = ( float ) ( MAX( fCumulative, 0.0 ) );
	profile.fHeightSlope = ( float ) ( fSlope );
	profile.fHeightIntercept = ( float ) ( fIntercept );
	profile.bValid = std::isfinite( profile.fHeightSlope ) && std::isfinite( profile.fHeightIntercept );
	return profile;
}

static float sampleBasinRimHeight( const t3dbasinProfile_t &rProfile, const t3dpathProjection_t &rProjection ) {
	if ( !rProfile.bValid ) {
		return rProjection.fPathHeight;
	}
	const float fDistance = CLAMP( rProjection.fPathDistance, 0.0f, rProfile.fPathLength );
	return rProfile.fHeightIntercept + rProfile.fHeightSlope * fDistance;
}

static AABB calculateBounds( const PackedVector3Array &arrPath, const t3dsettings_t &rSettings, const float fSpacing ) {
	Vector3 minimum(
		std::numeric_limits<real_t>::infinity(), TERRAIN3D_REGION_HEIGHT_MIN,
		std::numeric_limits<real_t>::infinity()
	);
	Vector3 maximum(
		-std::numeric_limits<real_t>::infinity(), TERRAIN3D_REGION_HEIGHT_MAX,
		-std::numeric_limits<real_t>::infinity()
	);
	for ( int index = 0; index < arrPath.size(); ++index ) {
		const Vector3 point = arrPath[index];
		minimum.x = MIN( minimum.x, point.x );
		minimum.z = MIN( minimum.z, point.z );
		maximum.x = MAX( maximum.x, point.x );
		maximum.z = MAX( maximum.z, point.z );
	}
	const float fRadius = MAX( rSettings.fWidth * 0.5f, fSpacing );
	minimum.x -= fRadius + fSpacing;
	minimum.z -= fRadius + fSpacing;
	maximum.x += fRadius + fSpacing;
	maximum.z += fRadius + fSpacing;
	return AABB( minimum, maximum - minimum );
}

static Dictionary captureSnapshot( const t3dregionEdit_t &rEdit, const bool bExisted ) {
	Dictionary snapshot;
	snapshot["location"] = rEdit.location;
	snapshot["exists"] = bExisted;
	if ( !bExisted || rEdit.pRegion.is_null() ) {
		return snapshot;
	}
	const Ref<Image> pHeight = rEdit.pRegion->getHeightMap();
	const Ref<Image> pControl = rEdit.pRegion->getControlMap();
	snapshot["height"] = pHeight.is_valid() ? Variant( pHeight->duplicate() ) : Variant();
	snapshot["control"] = pControl.is_valid() ? Variant( pControl->duplicate() ) : Variant();
	return snapshot;
}

static bool validateHeightMap( const Image *pHeight, const int nRegionSize ) {
	if ( !pHeight || pHeight->get_width() != nRegionSize || pHeight->get_height() != nRegionSize ) {
		return false;
	}
	const Image::Format format = pHeight->get_format();
	if ( format != Image::FORMAT_R16 && format != Image::FORMAT_RF ) {
		return false;
	}
	const size_t expected_bytes = Terrain3DRegionCodec::expectedHeightByteCount( nRegionSize, format );
	return expected_bytes > 0u && ( size_t ) ( pHeight->get_data().size() ) == expected_bytes;
}

static bool validateControlMap( const Ref<Image> &rControl, const int nRegionSize ) {
	if ( rControl.is_null() ) {
		return true;
	}
	if ( rControl->get_width() != nRegionSize || rControl->get_height() != nRegionSize ||
		rControl->get_format() != Image::FORMAT_RF ) {
		return false;
	}
	const int64_t expected_bytes = ( int64_t ) ( nRegionSize ) * ( int64_t ) ( nRegionSize ) * ( int64_t ) ( sizeof( float ) );
	return rControl->get_data().size() == expected_bytes;
}

static void removeCreatedRegions( Terrain3DData *pData, const eastl::vector<t3dregionEdit_t> &rEdits ) {
	if ( !pData ) {
		return;
	}
	for ( const t3dregionEdit_t &edit : rEdits ) {
		if ( !edit.bExistedBefore && pData->hasRegion( edit.location ) ) {
			pData->removeRegionl( edit.location, true );
		}
	}
}

static uint32_t updateMaterial( const uint32_t nSource, const t3dsettings_t &rSettings ) {
	const uint8_t blend = ( uint8_t ) ( CLAMP( Math::round( rSettings.fMaterialBlend * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
	return ( nSource & ~TERRAIN3D_LANDFORM_MATERIAL_MASK ) | encBase( ( uint8_t ) ( rSettings.base_texture ) ) |
								 encOverlay( ( uint8_t ) ( rSettings.overlay_texture ) ) | encBlend( blend ) | encAuto( false );
}

static float neighborValue(
	const eastl::vector<float> &fValues, const int nSize, const int iX, const int iY,
	const float fFallback
) {
	if ( iX < 0 || iY < 0 || iX >= nSize || iY >= nSize ) {
		return fFallback;
	}
	return fValues[( size_t ) ( iY * nSize + iX )];
}

static float morphologyTarget(
	const eastl::vector<float> &fValues, const int nSize, const int iX, const int iY,
	const Terrain3DLandformBaker::t3dmorphologyMode_t mode
) {
	const float fSource = fValues[( size_t ) ( iY * nSize + iX )];
	float fMinimum = fSource;
	float fMaximum = fSource;
	float fSum = 0.0f;
	int count = 0;
	for ( int offset_y = -1; offset_y <= 1; ++offset_y ) {
		for ( int offset_x = -1; offset_x <= 1; ++offset_x ) {
			const float fValue = neighborValue( fValues, nSize, iX + offset_x, iY + offset_y, fSource );
			fMinimum = MIN( fMinimum, fValue );
			fMaximum = MAX( fMaximum, fValue );
			fSum += fValue;
			++count;
		}
	}
	const float fAverage = fSum / ( float ) ( count );
	switch ( mode ) {
		case Terrain3DLandformBaker::MORPH_FILL_PITS:
			return fMaximum;
		case Terrain3DLandformBaker::MORPH_RIDGE_SHARPEN:
			return fSource + MAX( fSource - fAverage, 0.0f );
		case Terrain3DLandformBaker::MORPH_VALLEY_SHARPEN:
			return fSource - MAX( fAverage - fSource, 0.0f );
		case Terrain3DLandformBaker::MORPH_PLATEAU_CLEAN:
			return Math::abs( fSource - fAverage ) < 2.0f ? fAverage : fSource;
		case Terrain3DLandformBaker::MORPH_EXPAND_PEAKS:
		default:
			return fMaximum;
	}
}

static float thermalTarget(
	const eastl::vector<float> &fValues, const int nSize, const int iX, const int iY,
	const float fSpacing, const float fAngleDegrees
) {
	const float fSource = fValues[( size_t ) ( iY * nSize + iX )];
	const float fMaximumDelta = Math::tan( Math::deg_to_rad( fAngleDegrees ) ) * fSpacing;
	float fSum = 0.0f;
	int count = 0;
	for ( int offset_y = -1; offset_y <= 1; ++offset_y ) {
		for ( int offset_x = -1; offset_x <= 1; ++offset_x ) {
			if ( offset_x == 0 && offset_y == 0 ) {
				continue;
			}
			const float fNeighbor = neighborValue( fValues, nSize, iX + offset_x, iY + offset_y, fSource );
			const float fDelta = fNeighbor - fSource;
			if ( Math::abs( fDelta ) > fMaximumDelta ) {
				fSum += fNeighbor - Math::sign( fDelta ) * fMaximumDelta;
				++count;
			}
		}
	}
	return count > 0 ? fSum / ( float ) ( count ) : fSource;
}

typedef enum t3dheightPolicy_e {
	FREE,
	RAISE_ONLY,
	LOWER_ONLY,
} t3dheightPolicy_t;

static t3dheightPolicy_t getHeightPolicy( const t3dsettings_t &rSettings ) {
	switch ( rSettings.mode ) {
		case Terrain3DLandformBaker::RIDGE:
		case Terrain3DLandformBaker::SCARP:
		case Terrain3DLandformBaker::PLATEAU:
		case Terrain3DLandformBaker::TALUS:
		case Terrain3DLandformBaker::DUNE:
		case Terrain3DLandformBaker::OCCLUSION:
			return t3dheightPolicy_t::RAISE_ONLY;
		case Terrain3DLandformBaker::VALLEY:
		case Terrain3DLandformBaker::BASIN:
		case Terrain3DLandformBaker::PASS:
			return t3dheightPolicy_t::LOWER_ONLY;
		case Terrain3DLandformBaker::MORPHOLOGY:
			switch ( rSettings.morphology_mode ) {
				case Terrain3DLandformBaker::MORPH_EXPAND_PEAKS:
				case Terrain3DLandformBaker::MORPH_FILL_PITS:
				case Terrain3DLandformBaker::MORPH_RIDGE_SHARPEN:
					return t3dheightPolicy_t::RAISE_ONLY;
				case Terrain3DLandformBaker::MORPH_VALLEY_SHARPEN:
					return t3dheightPolicy_t::LOWER_ONLY;
				case Terrain3DLandformBaker::MORPH_PLATEAU_CLEAN:
				default:
					return t3dheightPolicy_t::FREE;
			}
		default:
			return t3dheightPolicy_t::FREE;
	}
}

static float enforceHeightPolicy( const t3dsettings_t &rSettings, const float fSource, const float fTarget ) {
	const float fBlended = Math::lerp( fSource, fTarget, rSettings.fStrength );
	switch ( getHeightPolicy( rSettings ) ) {
		case t3dheightPolicy_t::RAISE_ONLY:
			return MAX( fSource, fBlended );
		case t3dheightPolicy_t::LOWER_ONLY:
			return MIN( fSource, fBlended );
		case t3dheightPolicy_t::FREE:
		default:
			return fBlended;
	}
}

static float evaluateHeight(
	const t3dsettings_t &rSettings, const t3dpathProjection_t &rProjection, const Vector2 &rWorld,
	const Vector2 &rAreaCenter, const float fAreaRadius, const float fSource,
	const float fNeighborTarget, const t3dbasinProfile_t &rBasinProfile
) {
	const float fHalfWidth = MAX( rSettings.fWidth * 0.5f, 0.5f );
	const float fWeight = profileWeight( rProjection.distance, fHalfWidth, rSettings.fCrestWidth, rSettings.fFalloff );
	const float fGridNoise = signedNoise( ( int ) ( Math::floor( rWorld.x / rSettings.noise_scale ) ),
										 ( int ) ( Math::floor( rWorld.y / rSettings.noise_scale ) ), rSettings.seed );
	const float fNoiseFactor = 1.0f + fGridNoise * rSettings.noise;
	float fTarget = fSource;

	switch ( rSettings.mode ) {
		case Terrain3DLandformBaker::RIDGE:
			fTarget = fSource + rSettings.fAmplitude * fWeight * fNoiseFactor;
			break;
		case Terrain3DLandformBaker::VALLEY:
			fTarget = fSource - rSettings.fAmplitude * fWeight * fNoiseFactor;
			break;
		case Terrain3DLandformBaker::SCARP: {
			const float fTransition = MAX( 0.025f, 0.18f * ( 1.0f - Math::abs( rSettings.fAsymmetry ) * 0.75f ) );
			const float fRaisedDistance = rProjection.fSignedDistance * rSettings.fCliffSide;
			const float fSide = CLAMP( fRaisedDistance / fHalfWidth - rSettings.fAsymmetry * 0.25f, -1.0f, 1.0f );
			const float fStep = smooth01( ( fSide + fTransition ) / ( fTransition * 2.0f ) );
			fTarget = fSource + fStep * rSettings.fAmplitude * fWeight * fNoiseFactor;
			break;
		}
		case Terrain3DLandformBaker::BASIN: {
			// The stroke is the basin spine. Its width creates a rounded capsule,
			// while the fitted longitudinal grade removes local cursor-height noise.
			// This produces one continuous floor instead of the old bounding-box
			// circle whose nearest-segment heights could jump across the depression.
			const float fBasinWeight =
				profileWeight( rProjection.distance, fHalfWidth, rSettings.fBasinFloorWidth, rSettings.fFalloff );
			if ( fBasinWeight > 0.0f ) {
				const float fRimHeight = sampleBasinRimHeight( rBasinProfile, rProjection );
				const float fFloorHeight = fRimHeight - rSettings.fAmplitude;
				fTarget = Math::lerp( fSource, fFloorHeight, fBasinWeight );
			}
			break;
		}
		case Terrain3DLandformBaker::PLATEAU: {
			const float normalized = rWorld.distance_to( rAreaCenter ) / MAX( fAreaRadius, 1.0f );
			if ( normalized <= 1.0f ) {
				const float fEdge = 1.0f - smooth01( MAX( 0.0f, normalized - rSettings.fCrestWidth ) /
												 MAX( 1.0f - rSettings.fCrestWidth, TERRAIN3D_LANDFORM_EPSILON ) );
				fTarget = Math::lerp( fSource, rProjection.fPathHeight + rSettings.fAmplitude, fEdge );
			}
			break;
		}
		case Terrain3DLandformBaker::TERRACE: {
			const float fBase = rProjection.fPathHeight;
			const float fQuantized = fBase + Math::round( ( fSource - fBase ) / rSettings.fTerraceStep ) * rSettings.fTerraceStep;
			fTarget = Math::lerp( fSource, fQuantized, fWeight );
			break;
		}
		case Terrain3DLandformBaker::ROAD: {
			const float fLateral = CLAMP( Math::abs( rProjection.fSignedDistance ) / fHalfWidth, 0.0f, 1.0f );
			const float fCrown = rSettings.fRoadCrown * ( 1.0f - fLateral * fLateral );
			fTarget = Math::lerp( fSource, rProjection.fPathHeight + fCrown, fWeight );
			break;
		}
		case Terrain3DLandformBaker::PASS:
			fTarget = Math::lerp( fSource, rProjection.fPathHeight - rSettings.fAmplitude, fWeight );
			break;
		case Terrain3DLandformBaker::TALUS: {
			const float fPreferredSide = rSettings.fAsymmetry < 0.0f ? -1.0f : 1.0f;
			const float fSide = MAX( 0.0f, rProjection.fSignedDistance * fPreferredSide / fHalfWidth );
			const float fApron = ( 1.0f - CLAMP( fSide, 0.0f, 1.0f ) ) * fWeight;
			fTarget = fSource + rSettings.fAmplitude * fApron * ( 0.7f + 0.3f * fNoiseFactor );
			break;
		}
		case Terrain3DLandformBaker::HYDRAULIC_CHANNEL: {
			const float normalized = rProjection.distance / fHalfWidth;
			const float fChannel = Math::exp( -normalized * normalized * 10.0f );
			const float fBank = Math::exp( -Math::pow( normalized - 0.62f, 2.0f ) * 38.0f );
			fTarget = fSource - rSettings.fAmplitude * fChannel * fNoiseFactor + rSettings.fAmplitude * 0.18f * fBank;
			break;
		}
		case Terrain3DLandformBaker::THERMAL_RELAXATION:
			fTarget = Math::lerp( fSource, fNeighborTarget, fWeight );
			break;
		case Terrain3DLandformBaker::DUNE: {
			const Vector2 wind = rProjection.tangent.length_squared() > TERRAIN3D_LANDFORM_EPSILON ? rProjection.tangent.normalized()
																				 : Vector2( 1.0f, 0.0f );
			float fPhase = Math::fmod( rWorld.dot( wind ) / rSettings.fDuneSpacing, 1.0f );
			if ( fPhase < 0.0f ) {
				fPhase += 1.0f;
			}
			const float fDune = fPhase < 0.78f ? fPhase / 0.78f : ( 1.0f - fPhase ) / 0.22f;
			fTarget = fSource + rSettings.fAmplitude * smooth01( fDune ) * fWeight * fNoiseFactor;
			break;
		}
		case Terrain3DLandformBaker::SHORELINE: {
			const float fDesired = rSettings.water_level + rProjection.fSignedDistance * rSettings.fShoreSlope;
			fTarget = Math::lerp( fSource, fDesired, fWeight );
			break;
		}
		case Terrain3DLandformBaker::MORPHOLOGY:
			fTarget = Math::lerp( fSource, fNeighborTarget, fWeight );
			break;
		case Terrain3DLandformBaker::OCCLUSION: {
			const float fRidge = rSettings.fAmplitude * fWeight * fNoiseFactor;
			const float fSide = CLAMP( rProjection.fSignedDistance / fHalfWidth, -1.0f, 1.0f );
			fTarget = fSource + fRidge * ( 1.0f - 0.28f * MAX( fSide, 0.0f ) );
			break;
		}
		default:
			break;
	}
	return enforceHeightPolicy( rSettings, fSource, fTarget );
}

static PackedVector3Array smoothPath( const PackedVector3Array &arrPath, const int nPasses ) {
	if ( arrPath.size() < 3 || nPasses <= 0 ) {
		return arrPath;
	}
	PackedVector3Array current = arrPath;
	for ( int pass = 0; pass < nPasses; ++pass ) {
		PackedVector3Array next;
		next.resize( current.size() );
		next.set( 0, current[0] );
		for ( int index = 1; index + 1 < current.size(); ++index ) {
			next.set( index, current[index - 1] * 0.25f + current[index] * 0.5f + current[index + 1] * 0.25f );
		}
		next.set( current.size() - 1, current[current.size() - 1] );
		current = next;
	}
	return current;
}

static PackedVector3Array resamplePath( const PackedVector3Array &arrPath, const float fSpacing ) {
	PackedVector3Array result;
	if ( arrPath.is_empty() ) {
		return result;
	}
	result.push_back( arrPath[0] );
	for ( int index = 0; index + 1 < arrPath.size(); ++index ) {
		const Vector3 a = arrPath[index];
		const Vector3 b = arrPath[index + 1];
		const float fLength = segmentLengthXz( a, b );
		if ( fLength <= TERRAIN3D_LANDFORM_EPSILON ) {
			continue;
		}
		const int steps = MAX( 1, ( int ) ( Math::ceil( fLength / fSpacing ) ) );
		for ( int step = 1; step <= steps; ++step ) {
			result.push_back( a.lerp( b, ( float ) ( step ) / ( float ) ( steps ) ) );
		}
	}
	return result;
}

static void addTriangle(
	const Ref<SurfaceTool> &rSurface, const Vector3 &vA, const Vector3 &rB,
	const Vector3 &rC, const Vector2 &rUvA, const Vector2 &rUvB, const Vector2 &rUvC
) {
	rSurface->set_uv( rUvA );
	rSurface->add_vertex( vA );
	rSurface->set_uv( rUvB );
	rSurface->add_vertex( rB );
	rSurface->set_uv( rUvC );
	rSurface->add_vertex( rC );
}

static void addQuad(
	const Ref<SurfaceTool> &rSurface, const Vector3 &rATop, const Vector3 &rABottom,
	const Vector3 &rBBottom, const Vector3 &rBTop, const Vector2 &rUvATop,
	const Vector2 &rUvABottom, const Vector2 &rUvBBottom, const Vector2 &rUvBTop
) {
	addTriangle( rSurface, rATop, rABottom, rBBottom, rUvATop, rUvABottom, rUvBBottom );
	addTriangle( rSurface, rATop, rBBottom, rBTop, rUvATop, rUvBBottom, rUvBTop );
}

static Ref<StandardMaterial3D> createCliffMaterial(
	Terrain3D *pTerrain, const t3dsettings_t &rSettings,
	Dictionary &rCache
) {
	String cache_key = "default";
	Ref<Terrain3DTextureAsset> pAsset;
	if ( pTerrain && pTerrain->getAssets().is_valid() ) {
		pAsset = pTerrain->getAssets()->getTextureAsset( rSettings.base_texture );
		cache_key = String::num_uint64( ( uint64_t ) ( pTerrain->get_instance_id() ) ) + ":" +
			String::num_int64( rSettings.base_texture ) + ":" +
			String::num_uint64( pAsset.is_valid() ? ( uint64_t ) ( pAsset->get_instance_id() ) : 0u );
	}
	if ( rCache.has( cache_key ) ) {
		const Ref<StandardMaterial3D> s_nCachedSignature = rCache[cache_key];
		if ( s_nCachedSignature.is_valid() ) {
			return s_nCachedSignature;
		}
	}

	Ref<StandardMaterial3D> pMaterial;
	pMaterial.instantiate();
	pMaterial->set_roughness( 1.0f );
	pMaterial->set_cull_mode( BaseMaterial3D::CULL_DISABLED );
	pMaterial->set_texture_filter( BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC );
	if ( pAsset.is_valid() ) {
		pMaterial->set_albedo( pAsset->getAlbedoColor() );
		pMaterial->set_texture( BaseMaterial3D::TEXTURE_ALBEDO, pAsset->getAlbedoTexture() );
		const Ref<Texture2D> pNormalTexture = pAsset->getNormalTexture();
		if ( pNormalTexture.is_valid() ) {
			pMaterial->set_texture( BaseMaterial3D::TEXTURE_NORMAL, pNormalTexture );
			pMaterial->set_feature( BaseMaterial3D::FEATURE_NORMAL_MAPPING, true );
			pMaterial->set_normal_scale( pAsset->getNormalDepth() );
		}
		pMaterial->set_metallic( pAsset->getMetallic() );
		pMaterial->set_roughness( CLAMP( 0.5f + pAsset->getRoughness(), 0.0f, 1.0f ) );
	}
	rCache[cache_key] = pMaterial;
	return pMaterial;
}

static eastl::vector<int> getValidMeshIds( Terrain3D *pTerrain, const eastl::vector<int> &nIds ) {
	eastl::vector<int> valid;
	if ( !pTerrain || pTerrain->getAssets().is_null() ) {
		return valid;
	}
	valid.reserve( nIds.size() );
	for ( const int mesh_id : nIds ) {
		if ( mesh_id < 0 || mesh_id >= pTerrain->getAssets()->getMeshCount() ) {
			continue;
		}
		const Ref<Terrain3DMeshAsset> pAsset = pTerrain->getAssets()->getMeshAsset( mesh_id );
		if ( pAsset.is_valid() && pAsset->isEnabled() && pAsset->getMesh( 0 ).is_valid() ) {
			valid.push_back( mesh_id );
		}
	}
	return valid;
}

static float moduleScalePenalty( const float fScale, const float fMinScale, const float fMaxScale ) {
	const float fSafeScale = MAX( fScale, TERRAIN3D_LANDFORM_EPSILON );
	float fPenalty = Math::abs( Math::log( fSafeScale ) );
	if ( fScale < fMinScale ) {
		fPenalty += 100.0f + ( fMinScale - fScale ) * 100.0f;
	} else if ( fScale > fMaxScale ) {
		fPenalty += 100.0f + ( fScale - fMaxScale ) * 100.0f;
	}
	return fPenalty;
}

static int chooseModuleId(
	Terrain3D *pTerrain, const eastl::vector<int> &nPool, const float fTargetLength,
	const float fTargetHeight, const float fTargetDepth, const float fMinScale,
	const float fMaxScale, const int iSegmentIndex, const Vector3 &rCenter,
	const int nSeed, const int nPreviousId
) {
	if ( !pTerrain || pTerrain->getAssets().is_null() || nPool.empty() ) {
		return -1;
	}

	eastl::vector<t3dlandformModuleCandidate_t> candidates;
	candidates.reserve( nPool.size() );
	const int world_x = ( int ) ( Math::floor( rCenter.x * 0.25f ) );
	const int world_z = ( int ) ( Math::floor( rCenter.z * 0.25f ) );
	const uint32_t base_hash = hashU32( ( uint32_t ) ( world_x ) ^ ( ( uint32_t ) ( world_z ) * TERRAIN3D_HASH_GOLDEN_RATIO_32 ) ^
		( ( uint32_t ) ( iSegmentIndex ) * TERRAIN3D_HASH_MIX_32 ) ^ ( uint32_t ) ( nSeed ) );

	for ( const int mesh_id : nPool ) {
		const Ref<Terrain3DMeshAsset> pAsset = pTerrain->getAssets()->getMeshAsset( mesh_id );
		const Ref<Mesh> pSourceMesh = pAsset.is_valid() ? pAsset->getMesh( 0 ) : Ref<Mesh>();
		if ( pSourceMesh.is_null() ) {
			continue;
		}
		const Vector3 size = pSourceMesh->get_aabb().size;
		if ( size.x <= TERRAIN3D_LANDFORM_EPSILON || size.y <= TERRAIN3D_LANDFORM_EPSILON || size.z <= TERRAIN3D_LANDFORM_EPSILON ) {
			continue;
		}
		const float fScaleX = fTargetLength / size.x;
		const float fScaleY = fTargetHeight / size.y;
		const float fScaleZ = fTargetDepth / size.z;
		float score = moduleScalePenalty( fScaleX, fMinScale, fMaxScale );
		score += moduleScalePenalty( fScaleY, fMinScale, fMaxScale ) * 1.5f;
		score += moduleScalePenalty( fScaleZ, fMinScale, fMaxScale ) * 0.5f;
		const uint32_t tie_hash = hashU32( base_hash ^ ( ( uint32_t ) ( mesh_id ) * 0x27d4eb2du ) );
		score += ( float ) ( tie_hash & 0xFFFFu ) / 65535.0f * 0.05f;
		candidates.push_back( {mesh_id, score} );
	}
	if ( candidates.empty() ) {
		return -1;
	}
	std::sort( candidates.begin(), candidates.end(), landformModuleCandidateLess );

	const float fViableScore = candidates.front().fScore + 0.75f;
	size_t viable_count = 1;
	while ( viable_count < candidates.size() && viable_count < 3 && candidates[viable_count].fScore <= fViableScore ) {
		++viable_count;
	}
	const size_t offset = ( size_t ) ( base_hash % ( uint32_t ) ( viable_count ) );
	for ( size_t attempt = 0; attempt < viable_count; ++attempt ) {
		const t3dlandformModuleCandidate_t &candidate = candidates[( offset + attempt ) % viable_count];
		if ( viable_count == 1 || candidate.meshId != nPreviousId ) {
			return candidate.meshId;
		}
	}
	for ( const t3dlandformModuleCandidate_t &candidate : candidates ) {
		if ( candidate.meshId != nPreviousId ) {
			return candidate.meshId;
		}
	}
	return candidates.front().meshId;
}

static Vector2 segmentTangent( const PackedVector3Array &rSamples, const int iIndex ) {
	if ( rSamples.size() < 2 ) {
		return Vector2( 1.0f, 0.0f );
	}
	const int a_index = CLAMP( iIndex, 0, rSamples.size() - 2 );
	const Vector3 a = rSamples[a_index];
	const Vector3 b = rSamples[a_index + 1];
	Vector2 tangent( b.x - a.x, b.z - a.z );
	return tangent.length_squared() > TERRAIN3D_LANDFORM_EPSILON ? tangent.normalized() : Vector2( 1.0f, 0.0f );
}


} // namespace

typedef struct t3dlandformDecodeContext_s {
	eastl::vector<t3dregionEdit_t> *edits = nullptr;
	size_t pixel_count = 0u;
	std::atomic<bool> *pInvalidSource = nullptr;
} t3dlandformDecodeContext_t;

static void decodeLandformRegions( void *pContext, const size_t nBegin, const size_t nEnd, int32_t ) {
	t3dlandformDecodeContext_t *context = ( t3dlandformDecodeContext_t * ) ( pContext );
	for ( size_t edit_index = nBegin; edit_index < nEnd; ++edit_index ) {
		t3dregionEdit_t &edit = ( *context->edits )[edit_index];
		edit.source_values.resize( context->pixel_count );
		const uint8_t *pHeightRead = edit.height_snapshot.data();
		for ( size_t index = 0; index < context->pixel_count; ++index ) {
			const float fValue = Terrain3DRegionCodec::readHeightBytes( pHeightRead, ( int ) ( index ), edit.height_format );
			if ( !std::isfinite( fValue ) ) {
				edit.bCalculationValid = false;
				context->pInvalidSource->store( true, std::memory_order_relaxed );
				break;
			}
			edit.source_values[index] = fValue;
		}
		edit.height_snapshot.clear();
	}
}

typedef struct t3dlandformCalculateContext_s {
	eastl::vector<t3dregionEdit_t> *edits = nullptr;
	const t3dsettings_t *pSettings = nullptr;
	const PackedVector3Array *pOperationPath = nullptr;
	const AABB *pEditedArea = nullptr;
	const t3dbasinProfile_t *pBasinProfile = nullptr;
	Vector2 areaCenter = Vector2();
	float fAreaRadius = 0.0f;
	float fSpacing = 1.0f;
	int nRegionSize = 0;
	std::atomic<bool> *pInvalidOutput = nullptr;
} t3dlandformCalculateContext_t;

static void calculateLandformRegions( void *pContext, const size_t nBegin, const size_t nEnd, int32_t ) {
	t3dlandformCalculateContext_t *context = ( t3dlandformCalculateContext_t * ) ( pContext );
	const t3dsettings_t &settings = *context->pSettings;
	const PackedVector3Array &operation_path = *context->pOperationPath;
	const AABB &edited_area = *context->pEditedArea;
	const t3dbasinProfile_t &basin_profile = *context->pBasinProfile;
	for ( size_t edit_index = nBegin; edit_index < nEnd; ++edit_index ) {
		t3dregionEdit_t &edit = ( *context->edits )[edit_index];
		const eastl::vector<float> &source_values = edit.source_values;
		eastl::vector<float> working_values = source_values;
		eastl::vector<float> next_values = working_values;
		const int relaxation_passes = settings.mode == Terrain3DLandformBaker::THERMAL_RELAXATION ? settings.iterations : 1;
		for ( int pass = 0; pass < relaxation_passes; ++pass ) {
			const eastl::vector<float> &pass_source = working_values;
			next_values = working_values;
			for ( int y = 0; y < context->nRegionSize; ++y ) {
				for ( int x = 0; x < context->nRegionSize; ++x ) {
					const float fWorldX = ( float ) ( edit.location.x * context->nRegionSize + x ) * context->fSpacing;
					const float fWorldZ = ( float ) ( edit.location.y * context->nRegionSize + y ) * context->fSpacing;
					if ( fWorldX < edited_area.position.x || fWorldX > edited_area.get_end().x ||
						fWorldZ < edited_area.position.z || fWorldZ > edited_area.get_end().z ) {
						continue;
					}
					const Vector2 world( fWorldX, fWorldZ );
					const t3dpathProjection_t projection = projectToPath( operation_path, world );
					const float fSource = pass_source[( size_t ) ( y * context->nRegionSize + x )];
					float fNeighborTarget = fSource;
					if ( settings.mode == Terrain3DLandformBaker::THERMAL_RELAXATION ) {
						fNeighborTarget = thermalTarget( pass_source, context->nRegionSize, x, y, context->fSpacing, settings.fAngleOfRepose );
					} else if ( settings.mode == Terrain3DLandformBaker::MORPHOLOGY ) {
						fNeighborTarget = morphologyTarget( pass_source, context->nRegionSize, x, y, settings.morphology_mode );
					}
					const float fEvaluated = evaluateHeight( settings, projection, world, context->areaCenter,
						context->fAreaRadius, fSource, fNeighborTarget, basin_profile );
					if ( !std::isfinite( fEvaluated ) ) {
						edit.bCalculationValid = false;
						context->pInvalidOutput->store( true, std::memory_order_relaxed );
						continue;
					}
					next_values[( size_t ) ( y * context->nRegionSize + x )] =
						CLAMP( fEvaluated, TERRAIN3D_REGION_HEIGHT_MIN, TERRAIN3D_REGION_HEIGHT_MAX );
				}
			}
			working_values.swap( next_values );
		}

		edit.output_values = std::move( working_values );
		for ( size_t index = 0; index < edit.output_values.size(); ++index ) {
			if ( !Math::is_equal_approx( source_values[index], edit.output_values[index] ) ) {
				edit.bHeightModified = true;
				break;
			}
		}
		if ( !edit.bCalculationValid ) {
			context->pInvalidOutput->store( true, std::memory_order_relaxed );
		}
	}
}

Dictionary Terrain3DLandformBaker::apply(
	Terrain3D *const pTerrain, const PackedVector3Array &arrPath,
	const Dictionary &dictSettings
) {
	Dictionary result;
	result["ok"] = false;
	if ( !pTerrain || !pTerrain->getData() ) {
		result["error"] = "Terrain3D is not initialized.";
		return result;
	}
	if ( arrPath.size() < 2 ) {
		result["error"] = "A landform stroke requires at least two points.";
		return result;
	}

	const t3dsettings_t settings = parseSettings( dictSettings );
	const PackedVector3Array operation_path = smoothPath( arrPath, 2 );
	const t3dbasinProfile_t basin_profile = buildBasinProfile( operation_path );
	Terrain3DData *const data = pTerrain->getData();
	const int region_size = pTerrain->getRegionSize();
	const float spacing = pTerrain->getVertexSpacing();
	const float fWorldRegionSize = ( float ) ( region_size ) * spacing;
	const AABB edited_area = calculateBounds( operation_path, settings, spacing );
	const Vector2 areaCenter(
		( edited_area.position.x + edited_area.get_end().x ) * 0.5f,
		( edited_area.position.z + edited_area.get_end().z ) * 0.5f
	);
	const float area_radius = MAX( Vector2( edited_area.size.x, edited_area.size.z ).length() * 0.25f,
							settings.fWidth * 0.5f );

	const int min_region_x = ( int ) ( Math::floor( edited_area.position.x / fWorldRegionSize ) );
	const int max_region_x = ( int ) ( Math::floor( ( edited_area.get_end().x - TERRAIN3D_LANDFORM_EPSILON ) / fWorldRegionSize ) );
	const int min_region_z = ( int ) ( Math::floor( edited_area.position.z / fWorldRegionSize ) );
	const int max_region_z = ( int ) ( Math::floor( ( edited_area.get_end().z - TERRAIN3D_LANDFORM_EPSILON ) / fWorldRegionSize ) );
	eastl::vector<t3dregionEdit_t> edits;
	bool bAddedRegions = false;
	const bool bAllowRegionCreation =
		settings.bAutoRegions && ( settings.mode != BASIN || settings.bBasinCreateRegions );
	for ( int region_x = min_region_x; region_x <= max_region_x; ++region_x ) {
		for ( int region_z = min_region_z; region_z <= max_region_z; ++region_z ) {
			const Vector2i location( region_x, region_z );
			const bool bExisted = data->hasRegion( location );
			if ( !bExisted && !bAllowRegionCreation ) {
				continue;
			}
			if ( !bExisted && data->addRegionBlank( location, false ).is_null() ) {
				continue;
			}
			bAddedRegions |= !bExisted;
			if ( bExisted && !data->ensureCpuRegionResident( location ) ) {
				removeCreatedRegions( data, edits );
				result["error"] = "A terrain region could not be paged into the CPU map cache; no terrain was changed.";
				return result;
			}
			const Ref<Terrain3DRegion> region = data->getRegion( location );
			if ( region.is_null() || region->isDeleted() ) {
				continue;
			}
			t3dregionEdit_t edit;
			edit.location = location;
			edit.pRegion = region;
			edit.bExistedBefore = bExisted;
			edit.height = region->getHeightMap();
			if ( settings.bPaintMaterial ) {
				edit.pControl = region->getControlMap();
			}
			edits.push_back( edit );
		}
	}
	if ( edits.empty() ) {
		result["error"] = "The stroke does not overlap an editable terrain region.";
		return result;
	}
	// Bulk-created regions do not publish their slot map until one consolidated
	// rebuild. The edit path below performs incremental layer uploads and therefore
	// requires the slot arrays to be coherent before the first commit.
	if ( bAddedRegions ) {
		data->updateMaps( Terrain3DRegion::TYPE_MAX, true, false );
	}

	// Snapshot engine-owned image bytes on the calling thread. Workers only touch
	// plain vectors, so the transactional calculation phase is free of Image and
	// resource access.
	const size_t pixel_count = ( size_t ) ( region_size ) * ( size_t ) ( region_size );
	for ( t3dregionEdit_t &edit : edits ) {
		Image *const pHeightMap = edit.pRegion->getMapPtrForWrite( Terrain3DRegion::TYPE_HEIGHT );
		if ( !validateHeightMap( pHeightMap, region_size ) ) {
			removeCreatedRegions( data, edits );
			result["error"] = "The landform overlapped a region with invalid height-map storage; no terrain was changed.";
			return result;
		}
		if ( settings.bPaintMaterial && !validateControlMap( edit.pRegion->getControlMap(), region_size ) ) {
			removeCreatedRegions( data, edits );
			result["error"] = "The landform overlapped a region with invalid control-map storage; no terrain was changed.";
			return result;
		}
		const PackedByteArray height_bytes = pHeightMap->get_data();
		edit.height_snapshot.resize( ( size_t ) ( height_bytes.size() ) );
		if ( !edit.height_snapshot.empty() ) {
			std::memcpy( edit.height_snapshot.data(), height_bytes.ptr(), edit.height_snapshot.size() );
		}
		edit.height_format = pHeightMap->get_format();
	}

	const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	const T3DParallelExecutor executor( ( int32_t ) ( MIN<size_t>( ( size_t ) ( hardware_threads ), edits.size() ) ) );
	std::atomic<bool> invalid_source{false};
	t3dlandformDecodeContext_t decode_context;
	decode_context.edits = &edits;
	decode_context.pixel_count = pixel_count;
	decode_context.pInvalidSource = &invalid_source;
	executor.runStatic( edits.size(), 1u, decodeLandformRegions, &decode_context );
	if ( invalid_source.load( std::memory_order_relaxed ) ) {
		removeCreatedRegions( data, edits );
		result["error"] = "The landform overlapped non-finite height data; no terrain was changed.";
		return result;
	}

	eastl::vector<Dictionary> before_snapshots;
	before_snapshots.reserve( edits.size() );
	for ( const t3dregionEdit_t &edit : edits ) {
		before_snapshots.push_back( captureSnapshot( edit, edit.bExistedBefore ) );
	}

	eastl::vector<Vector2i> height_locations;
	eastl::vector<Vector2i> control_locations;
	std::atomic<bool> invalid_output{false};
	t3dlandformCalculateContext_t calculate_context;
	calculate_context.edits = &edits;
	calculate_context.pSettings = &settings;
	calculate_context.pOperationPath = &operation_path;
	calculate_context.pEditedArea = &edited_area;
	calculate_context.pBasinProfile = &basin_profile;
	calculate_context.areaCenter = areaCenter;
	calculate_context.fAreaRadius = area_radius;
	calculate_context.fSpacing = spacing;
	calculate_context.nRegionSize = region_size;
	calculate_context.pInvalidOutput = &invalid_output;
	executor.runStatic( edits.size(), 1u, calculateLandformRegions, &calculate_context );
	if ( invalid_output.load( std::memory_order_relaxed ) ) {
		removeCreatedRegions( data, edits );
		result["error"] = "The landform produced a non-finite height; no terrain was changed.";
		return result;
	}
	for ( const t3dregionEdit_t &edit : edits ) {
		if ( edit.bHeightModified ) {
			height_locations.push_back( edit.location );
		}
	}

	if ( height_locations.empty() ) {
		for ( const t3dregionEdit_t &edit : edits ) {
			if ( !edit.bExistedBefore ) {
				data->removeRegionl( edit.location, true );
			}
		}
		result["error"] = "The selected operation produced no terrain changes.";
		return result;
	}

	// Do not retain blank regions that were touched only by the operation bounds.
	// They must also stay out of the undo snapshot, otherwise redo would recreate
	// terrain that the operation never modified.
	for ( const t3dregionEdit_t &edit : edits ) {
		if ( !edit.bExistedBefore && !edit.bHeightModified && !edit.bControlModified ) {
			data->removeRegionl( edit.location, true );
		}
	}

	// Publish only after every region has produced a finite output. This keeps the
	// operation transactional: a failure cannot leave half of a multi-region basin
	// written into the terrain data.
	for ( t3dregionEdit_t &edit : edits ) {
		if ( !edit.bHeightModified ) {
			continue;
		}
		Image *const pHeightMap = edit.pRegion->getMapPtrForWrite( Terrain3DRegion::TYPE_HEIGHT );
		Image *const pControlMap = settings.bPaintMaterial ? edit.pRegion->getMapPtrForWrite( Terrain3DRegion::TYPE_CONTROL ) : nullptr;
		PackedByteArray height_bytes = pHeightMap->get_data();
		uint8_t *const pHeightWrite = height_bytes.ptrw();
		const Image::Format height_format = pHeightMap->get_format();
		PackedByteArray control_bytes;
		uint8_t *pControlWrite = nullptr;
		if ( pControlMap ) {
			control_bytes = pControlMap->get_data();
			pControlWrite = control_bytes.ptrw();
		}
		for ( size_t index = 0; index < pixel_count; ++index ) {
			const float fSource = edit.source_values[index];
			const float fOutput = edit.output_values[index];
			if ( Math::is_equal_approx( fSource, fOutput ) ) {
				continue;
			}
			if ( height_format == Image::FORMAT_R16 ) {
				const uint16_t encoded = Terrain3DRegionCodec::encodeHeightU16( fOutput );
				std::memcpy( pHeightWrite + index * sizeof( uint16_t ), &encoded, sizeof( uint16_t ) );
			} else {
				std::memcpy( pHeightWrite + index * sizeof( float ), &fOutput, sizeof( float ) );
			}
			if ( pControlWrite ) {
				uint32_t source_control = TERRAIN3D_REGION_DEFAULT_CONTROL;
				std::memcpy( &source_control, pControlWrite + index * sizeof( uint32_t ), sizeof( uint32_t ) );
				const uint32_t replacement = updateMaterial( source_control, settings );
				if ( replacement != source_control ) {
					std::memcpy( pControlWrite + index * sizeof( uint32_t ), &replacement, sizeof( uint32_t ) );
					edit.bControlModified = true;
				}
			}
		}
		pHeightMap->set_data( region_size, region_size, pHeightMap->has_mipmaps(), height_format, height_bytes );
		if ( edit.bControlModified && pControlMap ) {
			pControlMap->set_data( region_size, region_size, pControlMap->has_mipmaps(), Image::FORMAT_RF, control_bytes );
		}
		if ( edit.bControlModified ) {
			control_locations.push_back( edit.location );
		}
	}

	uint32_t commit_flags = Terrain3DData::MAP_EDIT_UPDATE_INSTANCES | Terrain3DData::MAP_EDIT_SNAP;
	if ( pTerrain->getCollisionMode() == Terrain3DCollision::DYNAMIC_EDITOR ) {
		commit_flags |= Terrain3DData::MAP_EDIT_UPDATE_COLLISION;
	}
	data->commitMapEdits( Terrain3DRegion::TYPE_HEIGHT, height_locations, edited_area, commit_flags );
	if ( !control_locations.empty() ) {
		data->commitMapEdits( Terrain3DRegion::TYPE_CONTROL, control_locations, edited_area, Terrain3DData::MAP_EDIT_SNAP );
	}

	Array before;
	Array after;
	for ( size_t index = 0; index < edits.size(); ++index ) {
		const t3dregionEdit_t &edit = edits[index];
		if ( !edit.bHeightModified && !edit.bControlModified ) {
			continue;
		}
		before.push_back( before_snapshots[index] );
		after.push_back( captureSnapshot( edit, true ) );
	}

	result["ok"] = true;
	result["before"] = before;
	result["after"] = after;
	result["edited_area"] = edited_area;
	result["modified_regions"] = ( int ) ( height_locations.size() );
	if ( settings.bGenerateMesh && ( settings.mode == SCARP || settings.mode == OCCLUSION ) ) {
		if ( settings.geometry_mode == GEOMETRY_MODULAR_INSTANCED ) {
			result["modules"] = buildCliffModules( pTerrain, operation_path, dictSettings );
		} else if ( settings.geometry_mode == GEOMETRY_PROCEDURAL_UNIQUE ) {
			result["mesh"] = buildCliffMesh( pTerrain, operation_path, dictSettings );
		}
		if ( settings.bGenerateCollision && settings.geometry_mode != GEOMETRY_TERRAIN_ONLY ) {
			result["collision_mesh"] = buildCliffCollisionMesh( pTerrain, operation_path, dictSettings );
		}
	}
	return result;
}

void Terrain3DLandformBaker::restore( Terrain3D *const pTerrain, const Array &rSnapshots, const AABB &rEditedArea ) {
	if ( !pTerrain || !pTerrain->getData() ) {
		return;
	}
	Terrain3DData *const pData = pTerrain->getData();
	eastl::vector<Vector2i> height_locations;
	eastl::vector<Vector2i> control_locations;
	bool bAddedRegions = false;
	for ( int index = 0; index < rSnapshots.size(); ++index ) {
		const Dictionary snapshot = rSnapshots[index];
		if ( !snapshot.has( "location" ) || !snapshot.has( "exists" ) ) {
			continue;
		}
		const Vector2i location = snapshot["location"];
		const bool bShouldExist = ( bool ) ( snapshot["exists"] );
		if ( !bShouldExist ) {
			pData->removeRegionl( location, true );
			continue;
		}
		if ( !pData->hasRegion( location ) ) {
			if ( pData->addRegionBlank( location, false ).is_null() ) {
				continue;
			}
			bAddedRegions = true;
		}
		Ref<Terrain3DRegion> pRegion = pData->getRegion( location );
		if ( pRegion.is_null() ) {
			continue;
		}
		Ref<Image> pHeight;
		Ref<Image> pControl;
		const Variant height_value = snapshot.get( "height", Variant() );
		const Variant control_value = snapshot.get( "control", Variant() );
		if ( height_value.get_type() == Variant::OBJECT ) {
			pHeight = height_value;
		}
		if ( control_value.get_type() == Variant::OBJECT ) {
			pControl = control_value;
		}
		if ( pHeight.is_valid() ) {
			pRegion->setHeightMap( Ref<Image>( pHeight->duplicate() ) );
			height_locations.push_back( location );
		}
		if ( pControl.is_valid() ) {
			pRegion->setControlMap( Ref<Image>( pControl->duplicate() ) );
		} else {
			pRegion->setControlMap( Ref<Image>() );
		}
		control_locations.push_back( location );
	}
	if ( bAddedRegions ) {
		pData->updateMaps( Terrain3DRegion::TYPE_MAX, true, false );
	}
	if ( !height_locations.empty() ) {
		uint32_t flags = Terrain3DData::MAP_EDIT_UPDATE_INSTANCES | Terrain3DData::MAP_EDIT_SNAP;
		if ( pTerrain->getCollisionMode() == Terrain3DCollision::DYNAMIC_EDITOR ) {
			flags |= Terrain3DData::MAP_EDIT_UPDATE_COLLISION;
		}
		pData->commitMapEdits( Terrain3DRegion::TYPE_HEIGHT, height_locations, rEditedArea, flags );
	}
	if ( !control_locations.empty() ) {
		pData->commitMapEdits( Terrain3DRegion::TYPE_CONTROL, control_locations, rEditedArea, Terrain3DData::MAP_EDIT_SNAP );
	}
}

Dictionary Terrain3DLandformBaker::buildCliffModules(
	Terrain3D *const pTerrain,
	const PackedVector3Array &arrPath,
	const Dictionary &dictSettings
) const {
	Dictionary result;
	result["ok"] = false;
	result["placements"] = Dictionary();
	result["count"] = 0;
	if ( !pTerrain || !pTerrain->getData() || arrPath.size() < 2 || pTerrain->getAssets().is_null() ) {
		result["error"] = "Terrain3D and a two-point path are required for modular cliffs.";
		return result;
	}

	const t3dsettings_t settings = parseSettings( dictSettings );
	const eastl::vector<int> face_ids = getValidMeshIds( pTerrain, settings.module_mesh_ids );
	const eastl::vector<int> corner_ids = getValidMeshIds( pTerrain, settings.corner_mesh_ids );
	const eastl::vector<int> end_ids = getValidMeshIds( pTerrain, settings.end_mesh_ids );
	if ( face_ids.empty() ) {
		result["error"] = "Modular cliff mode requires at least one valid face mesh asset ID.";
		return result;
	}

	const PackedVector3Array samples = resamplePath( smoothPath( arrPath, 2 ), settings.fModuleLength );
	if ( samples.size() < 2 ) {
		result["error"] = "The path is too short for the selected module length.";
		return result;
	}

	T3DSampler sampler( pTerrain->getData()->getSamplingSourceInternal() );
	Dictionary placements;
	const float fVertexSpacing = MAX( pTerrain->getVertexSpacing(), 0.01f );
	int previous_mesh_id = -1;
	int module_count = 0;
	const int segment_count = samples.size() - 1;

	for ( int segment_index = 0; segment_index < segment_count; ++segment_index ) {
		const Vector3 a = samples[segment_index];
		const Vector3 b = samples[segment_index + 1];
		const float fHorizontalLength = segmentLengthXz( a, b );
		if ( fHorizontalLength <= TERRAIN3D_LANDFORM_EPSILON ) {
			continue;
		}
		const Vector3 center = a.lerp( b, 0.5f );
		const Vector2 tangent = segmentTangent( samples, segment_index );
		const Vector2 previous_tangent = segmentTangent( samples, MAX( segment_index - 1, 0 ) );
		const Vector2 next_tangent = segmentTangent( samples, MIN( segment_index + 1, segment_count - 1 ) );
		const float fPreviousTurn = Math::rad_to_deg(
			Math::acos( CLAMP( previous_tangent.dot( tangent ), -1.0f, 1.0f ) ) );
		const float fNextTurn = Math::rad_to_deg( Math::acos( CLAMP( tangent.dot( next_tangent ), -1.0f, 1.0f ) ) );
		const float fTurnAngle = MAX( fPreviousTurn, fNextTurn );
		const bool bIsEnd = segment_index == 0 || segment_index == segment_count - 1;
		const bool bIsCorner = fTurnAngle >= settings.fModuleCornerAngle;
		const eastl::vector<int> *pool = &face_ids;
		if ( bIsEnd && !end_ids.empty() ) {
			pool = &end_ids;
		} else if ( bIsCorner && !corner_ids.empty() ) {
			pool = &corner_ids;
		}

		const Vector2 raised_normal = Vector2( -tangent.y, tangent.x ) * settings.fCliffSide;
		const float fTransitionOffset = settings.fAsymmetry * settings.fWidth * 0.125f;
		const Vector2 face_a = Vector2( a.x, a.z ) + raised_normal * fTransitionOffset;
		const Vector2 face_b = Vector2( b.x, b.z ) + raised_normal * fTransitionOffset;
		const Vector2 face_center = face_a.lerp( face_b, 0.5f );
		const float fEdgeSample = MAX( fVertexSpacing * 1.5f, settings.fWidth * 0.10f );
		const float fHighA = sampleHeightOr(
			sampler, face_a + raised_normal * fEdgeSample, a.y + settings.fAmplitude );
		const float fHighB = sampleHeightOr(
			sampler, face_b + raised_normal * fEdgeSample, b.y + settings.fAmplitude );
		const float fLowA = sampleHeightOr( sampler, face_a - raised_normal * fEdgeSample, a.y );
		const float fLowB = sampleHeightOr( sampler, face_b - raised_normal * fEdgeSample, b.y );
		const float fBottomA = MIN( fLowA - settings.fMeshToe, fHighA - 0.25f );
		const float fBottomB = MIN( fLowB - settings.fMeshToe, fHighB - 0.25f );
		const float fFaceHeight = MAX( ( ( fHighA - fBottomA ) + ( fHighB - fBottomB ) ) * 0.5f, 0.25f );
		const float fTargetDepth = MAX( settings.fMeshDepth + settings.fMeshLip, fVertexSpacing );
		const float fTargetLength = Vector3( b.x - a.x, fBottomB - fBottomA, b.z - a.z ).length();
		const int mesh_id = chooseModuleId( pTerrain, *pool, fTargetLength, fFaceHeight, fTargetDepth,
			settings.fModuleMinScale, settings.fModuleMaxScale, segment_index, center, settings.seed,
			previous_mesh_id );
		if ( mesh_id < 0 ) {
			continue;
		}
		previous_mesh_id = mesh_id;

		const Ref<Terrain3DMeshAsset> pAsset = pTerrain->getAssets()->getMeshAsset( mesh_id );
		const Ref<Mesh> pSourceMesh = pAsset.is_valid() ? pAsset->getMesh( 0 ) : Ref<Mesh>();
		if ( pSourceMesh.is_null() ) {
			continue;
		}
		const AABB source_bounds = pSourceMesh->get_aabb();
		if ( source_bounds.size.x <= TERRAIN3D_LANDFORM_EPSILON || source_bounds.size.y <= TERRAIN3D_LANDFORM_EPSILON || source_bounds.size.z <= TERRAIN3D_LANDFORM_EPSILON ) {
			continue;
		}

		const float fScaleX = CLAMP( fTargetLength / source_bounds.size.x,
			settings.fModuleMinScale, settings.fModuleMaxScale );
		const float fScaleY = CLAMP( fFaceHeight / source_bounds.size.y,
			settings.fModuleMinScale, settings.fModuleMaxScale );
		const float fScaleZ = CLAMP( fTargetDepth / source_bounds.size.z,
			settings.fModuleMinScale, settings.fModuleMaxScale );
		const float fTangentDirection = settings.fCliffSide < 0.0f ? -1.0f : 1.0f;
		const Vector3 tangent_axis =
			Vector3( b.x - a.x, fBottomB - fBottomA, b.z - a.z ).normalized() * fTangentDirection;
		const Vector3 raisedAxis( raised_normal.x, 0.0f, raised_normal.y );
		Vector3 vertical_axis = raisedAxis.cross( tangent_axis );
		vertical_axis = vertical_axis.length_squared() > TERRAIN3D_LANDFORM_EPSILON ? vertical_axis.normalized() : V3_UP;
		const Basis basis( tangent_axis * fScaleX, vertical_axis * fScaleY, raisedAxis * fScaleZ );
		const Vector3 localAnchor(
			source_bounds.position.x + source_bounds.size.x * 0.5f,
			source_bounds.position.y, source_bounds.position.z
		);
		const Vector3 targetAnchor( face_center.x, ( fBottomA + fBottomB ) * 0.5f, face_center.y );
		const Transform3D transform( basis, targetAnchor - basis.xform( localAnchor ) );

		TypedArray<Transform3D> mesh_transforms;
		if ( placements.has( mesh_id ) ) {
			mesh_transforms = Array( placements[mesh_id] );
		}
		mesh_transforms.push_back( transform );
		placements[mesh_id] = mesh_transforms;
		++module_count;
	}

	if ( module_count == 0 ) {
		result["error"] = "No valid modular cliff placements could be generated from the selected assets.";
		return result;
	}
	result["ok"] = true;
	result["placements"] = placements;
	result["count"] = module_count;
	result["source_path"] = arrPath;
	return result;
}

Ref<ArrayMesh> Terrain3DLandformBaker::buildCliffCollisionMesh(
	Terrain3D *const pTerrain,
	const PackedVector3Array &arrPath,
	const Dictionary &dictSettings
) const {
	if ( !pTerrain || !pTerrain->getData() || arrPath.size() < 2 ) {
		return Ref<ArrayMesh>();
	}
	const t3dsettings_t settings = parseSettings( dictSettings );
	const PackedVector3Array samples = resamplePath( smoothPath( arrPath, 2 ), settings.fCollisionSpacing );
	if ( samples.size() < 2 ) {
		return Ref<ArrayMesh>();
	}

	typedef struct t3dcollisionSection_s {
		Vector3 top;
		Vector3 bottom;
	} t3dcollisionSection_t;
	eastl::vector<t3dcollisionSection_t> sections;
	sections.reserve( ( size_t ) ( samples.size() ) );
	T3DSampler sampler( pTerrain->getData()->getSamplingSourceInternal() );
	const float fVertexSpacing = MAX( pTerrain->getVertexSpacing(), 0.01f );

	for ( int index = 0; index < samples.size(); ++index ) {
		const Vector3 center = samples[index];
		const Vector3 previous = samples[MAX( index - 1, 0 )];
		const Vector3 next = samples[MIN( index + 1, samples.size() - 1 )];
		Vector2 tangent( next.x - previous.x, next.z - previous.z );
		tangent = tangent.length_squared() > TERRAIN3D_LANDFORM_EPSILON ? tangent.normalized() : Vector2( 1.0f, 0.0f );
		const Vector2 raised_normal = Vector2( -tangent.y, tangent.x ) * settings.fCliffSide;
		const Vector2 face_center = Vector2( center.x, center.z ) +
			raised_normal * ( settings.fAsymmetry * settings.fWidth * 0.125f );
		const float fEdgeSample = MAX( fVertexSpacing * 1.5f, settings.fWidth * 0.10f );
		const float fHighHeight = sampleHeightOr( sampler, face_center + raised_normal * fEdgeSample,
			center.y + settings.fAmplitude );
		const float fLowHeight = sampleHeightOr( sampler, face_center - raised_normal * fEdgeSample, center.y );
		const float fBottomHeight = MIN( fLowHeight - settings.fMeshToe, fHighHeight - 0.25f );
		sections.push_back( {Vector3( face_center.x, fHighHeight, face_center.y ),
			Vector3( face_center.x, fBottomHeight, face_center.y )} );
	}

	Ref<SurfaceTool> pSurface;
	pSurface.instantiate();
	pSurface->begin( Mesh::PRIMITIVE_TRIANGLES );
	for ( size_t index = 0; index + 1 < sections.size(); ++index ) {
		const t3dcollisionSection_t &a = sections[index];
		const t3dcollisionSection_t &b = sections[index + 1];
		addQuad( pSurface, a.top, a.bottom, b.bottom, b.top, V2_ZERO, V2_ZERO, V2_ZERO, V2_ZERO );
	}
	return pSurface->commit();
}

Ref<ArrayMesh> Terrain3DLandformBaker::buildCliffMesh(
	Terrain3D *const pTerrain, const PackedVector3Array &arrPath,
	const Dictionary &dictSettings
) const {
	if ( !pTerrain || !pTerrain->getData() || arrPath.size() < 2 ) {
		return Ref<ArrayMesh>();
	}
	const t3dsettings_t settings = parseSettings( dictSettings );
	const PackedVector3Array samples = resamplePath( smoothPath( arrPath, 2 ), settings.fMeshSampleSpacing );
	if ( samples.size() < 2 ) {
		return Ref<ArrayMesh>();
	}

	float pFUvScale = 0.1f;
	if ( pTerrain->getAssets().is_valid() ) {
		const Ref<Terrain3DTextureAsset> pAsset = pTerrain->getAssets()->getTextureAsset( settings.base_texture );
		if ( pAsset.is_valid() ) {
			pFUvScale = MAX( ( float ) ( pAsset->getUvScale() ), 0.001f );
		}
	}

	const float fVertexSpacing = MAX( pTerrain->getVertexSpacing(), 0.01f );
	T3DSampler sampler( pTerrain->getData()->getSamplingSourceInternal() );
	Ref<SurfaceTool> pSurface;
	pSurface.instantiate();
	pSurface->begin( Mesh::PRIMITIVE_TRIANGLES );
	float fCumulativeDistance = 0.0f;

	typedef struct t3dcrossSection_s {
		Vector3 high_ground;
		Vector3 front_top;
		Vector3 front_upper;
		Vector3 front_lower;
		Vector3 front_toe;
		Vector3 low_ground;
		float fU = 0.0f;
		float fFaceHeight = 1.0f;
	} t3dcrossSection_t;
	eastl::vector<t3dcrossSection_t> sections;
	sections.reserve( ( size_t ) ( samples.size() ) );

	for ( int index = 0; index < samples.size(); ++index ) {
		const Vector3 center = samples[index];
		const Vector3 previous = samples[MAX( index - 1, 0 )];
		const Vector3 next = samples[MIN( index + 1, samples.size() - 1 )];
		Vector2 tangent( next.x - previous.x, next.z - previous.z );
		if ( tangent.length_squared() <= TERRAIN3D_LANDFORM_EPSILON ) {
			tangent = Vector2( 1.0f, 0.0f );
		} else {
			tangent = tangent.normalized();
		}

		const Vector2 positiveNormal( -tangent.y, tangent.x );
		const Vector2 raised_normal = positiveNormal * settings.fCliffSide;
		const Vector2 centerXz( center.x, center.z );
		const float fTransitionOffset = settings.fAsymmetry * settings.fWidth * 0.125f;
		const Vector2 face_center = centerXz + raised_normal * fTransitionOffset;
		const float fEdgeSample = MAX( fVertexSpacing * 1.5f, settings.fWidth * 0.10f );
		const float fMaximumUpperDepth = MAX( fEdgeSample, settings.fWidth * 0.42f );
		const float fMaximumLowerDepth = MAX( fEdgeSample, settings.fWidth * 0.32f );
		const float fUpperDepth = CLAMP( settings.fMeshDepth + settings.fMeshLip, fEdgeSample, fMaximumUpperDepth );
		const float fLowerDepth = CLAMP( settings.fMeshDepth * 0.55f, fEdgeSample, fMaximumLowerDepth );
		const Vector2 high_near_xz = face_center + raised_normal * fEdgeSample;
		const Vector2 low_near_xz = face_center - raised_normal * fEdgeSample;
		const Vector2 high_ground_xz = face_center + raised_normal * fUpperDepth;
		const Vector2 low_ground_xz = face_center - raised_normal * fLowerDepth;


		const float fHighNear = sampleHeightOr( sampler, high_near_xz, center.y + settings.fAmplitude );
		const float fHighGroundHeight = sampleHeightOr( sampler, high_ground_xz, fHighNear );
		const float fLowNear = sampleHeightOr( sampler, low_near_xz, center.y );
		const float fLowGroundHeight = sampleHeightOr( sampler, low_ground_xz, fLowNear );
		const float fTopHeight = fHighNear;
		const float fFaceBottom = MIN( fLowNear - settings.fMeshToe, fTopHeight - 0.25f );
		const float face_height = fTopHeight - fFaceBottom;

		const float fNoiseSpacing = MAX( settings.fMeshSampleSpacing, 0.25f );
		const int noise_x = ( int ) ( Math::floor( center.x / fNoiseSpacing ) );
		const int noise_z = ( int ) ( Math::floor( center.z / fNoiseSpacing ) );
		const float fTopNoise = signedNoise( noise_x, noise_z, settings.seed ^ 0x517cc1b7 );
		const float fUpperNoise = signedNoise( noise_x, noise_z, settings.seed ^ 0x68bc21ebu );
		const float fLowerNoise = signedNoise( noise_x, noise_z, settings.seed ^ 0x02e5be93u );
		const float fToeNoise = signedNoise( noise_x, noise_z, settings.seed ^ 0x967a889bu );
		const float fMaxOffset = settings.fMeshDepth * 0.35f * settings.fMeshIrregularity;
		const Vector2 top_xz = face_center + raised_normal * ( fTopNoise * fMaxOffset * 0.25f );
		const Vector2 upper_face_xz = face_center + raised_normal * ( fUpperNoise * fMaxOffset );
		const Vector2 lower_face_xz = face_center + raised_normal * ( fLowerNoise * fMaxOffset );
		const Vector2 toe_xz = face_center + raised_normal * ( fToeNoise * fMaxOffset * 0.45f );
		const float fUpperBand = fFaceBottom + face_height * 0.70f + fUpperNoise * face_height * 0.035f;
		const float fLowerBand = fFaceBottom + face_height * 0.34f + fLowerNoise * face_height * 0.045f;
		if ( index > 0 ) {
			fCumulativeDistance += segmentLengthXz( samples[index - 1], center );
		}

		sections.push_back( {Vector3( high_ground_xz.x, fHighGroundHeight - 0.02f, high_ground_xz.y ),
							Vector3( top_xz.x, fTopHeight, top_xz.y ),
							Vector3( upper_face_xz.x, fUpperBand, upper_face_xz.y ),
							Vector3( lower_face_xz.x, fLowerBand, lower_face_xz.y ),
							Vector3( toe_xz.x, fFaceBottom, toe_xz.y ),
							Vector3( low_ground_xz.x, fLowGroundHeight - 0.02f, low_ground_xz.y ),
							fCumulativeDistance * pFUvScale, face_height} );
	}

	for ( size_t index = 0; index + 1 < sections.size(); ++index ) {
		const t3dcrossSection_t &a = sections[index];
		const t3dcrossSection_t &b = sections[index + 1];
		const float fAVUpper = ( a.front_top.y - a.front_upper.y ) * pFUvScale;
		const float fAVLower = ( a.front_top.y - a.front_lower.y ) * pFUvScale;
		const float fAVToe = a.fFaceHeight * pFUvScale;
		const float fAVGround = fAVToe + a.front_toe.distance_to( a.low_ground ) * pFUvScale;
		const float fBVUpper = ( b.front_top.y - b.front_upper.y ) * pFUvScale;
		const float fBVLower = ( b.front_top.y - b.front_lower.y ) * pFUvScale;
		const float fBVToe = b.fFaceHeight * pFUvScale;
		const float fBVGround = fBVToe + b.front_toe.distance_to( b.low_ground ) * pFUvScale;

		addQuad( pSurface, a.front_top, a.front_upper, b.front_upper, b.front_top, Vector2( a.fU, 0.0f ),
				 Vector2( a.fU, fAVUpper ), Vector2( b.fU, fBVUpper ), Vector2( b.fU, 0.0f ) );
		addQuad( pSurface, a.front_upper, a.front_lower, b.front_lower, b.front_upper, Vector2( a.fU, fAVUpper ),
				 Vector2( a.fU, fAVLower ), Vector2( b.fU, fBVLower ), Vector2( b.fU, fBVUpper ) );
		addQuad( pSurface, a.front_lower, a.front_toe, b.front_toe, b.front_lower, Vector2( a.fU, fAVLower ),
				 Vector2( a.fU, fAVToe ), Vector2( b.fU, fBVToe ), Vector2( b.fU, fBVLower ) );
		addQuad( pSurface, a.high_ground, a.front_top, b.front_top, b.high_ground,
				 Vector2( a.fU, settings.fMeshDepth * pFUvScale ), Vector2( a.fU, 0.0f ), Vector2( b.fU, 0.0f ),
				 Vector2( b.fU, settings.fMeshDepth * pFUvScale ) );
		addQuad( pSurface, a.front_toe, a.low_ground, b.low_ground, b.front_toe, Vector2( a.fU, fAVToe ),
				 Vector2( a.fU, fAVGround ), Vector2( b.fU, fBVGround ), Vector2( b.fU, fBVToe ) );
	}

	const t3dcrossSection_t &first = sections.front();
	const t3dcrossSection_t &last = sections.back();
	addTriangle( pSurface, first.high_ground, first.front_upper, first.front_top, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.5f ), Vector2( 1.0f, 0.0f ) );
	addTriangle( pSurface, first.high_ground, first.front_lower, first.front_upper, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.8f ), Vector2( 0.5f, 0.5f ) );
	addTriangle( pSurface, first.high_ground, first.front_toe, first.front_lower, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 0.9f ), Vector2( 0.5f, 0.8f ) );
	addTriangle( pSurface, first.high_ground, first.low_ground, first.front_toe, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 1.0f ), Vector2( 1.0f, 0.9f ) );
	addTriangle( pSurface, last.high_ground, last.front_top, last.front_upper, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 0.0f ), Vector2( 0.5f, 0.5f ) );
	addTriangle( pSurface, last.high_ground, last.front_upper, last.front_lower, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.5f ), Vector2( 0.5f, 0.8f ) );
	addTriangle( pSurface, last.high_ground, last.front_lower, last.front_toe, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.8f ), Vector2( 1.0f, 0.9f ) );
	addTriangle( pSurface, last.high_ground, last.front_toe, last.low_ground, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 0.9f ), Vector2( 1.0f, 1.0f ) );

	pSurface->generate_normals();
	pSurface->generate_tangents();
	Ref<ArrayMesh> pMesh = pSurface->commit();
	if ( pMesh.is_valid() && pMesh->get_surface_count() > 0 ) {
		pMesh->surface_set_material( 0, createCliffMaterial( pTerrain, settings, m_materialCache ) );
	}
	return pMesh;
}

void Terrain3DLandformBaker::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( RIDGE );
	BIND_ENUM_CONSTANT( VALLEY );
	BIND_ENUM_CONSTANT( SCARP );
	BIND_ENUM_CONSTANT( BASIN );
	BIND_ENUM_CONSTANT( PLATEAU );
	BIND_ENUM_CONSTANT( TERRACE );
	BIND_ENUM_CONSTANT( ROAD );
	BIND_ENUM_CONSTANT( PASS );
	BIND_ENUM_CONSTANT( TALUS );
	BIND_ENUM_CONSTANT( HYDRAULIC_CHANNEL );
	BIND_ENUM_CONSTANT( THERMAL_RELAXATION );
	BIND_ENUM_CONSTANT( DUNE );
	BIND_ENUM_CONSTANT( SHORELINE );
	BIND_ENUM_CONSTANT( MORPHOLOGY );
	BIND_ENUM_CONSTANT( OCCLUSION );
	BIND_ENUM_CONSTANT( MODE_MAX );

	BIND_ENUM_CONSTANT( MORPH_EXPAND_PEAKS );
	BIND_ENUM_CONSTANT( MORPH_FILL_PITS );
	BIND_ENUM_CONSTANT( MORPH_RIDGE_SHARPEN );
	BIND_ENUM_CONSTANT( MORPH_VALLEY_SHARPEN );
	BIND_ENUM_CONSTANT( MORPH_PLATEAU_CLEAN );
	BIND_ENUM_CONSTANT( MORPH_MAX );

	BIND_ENUM_CONSTANT( GEOMETRY_TERRAIN_ONLY );
	BIND_ENUM_CONSTANT( GEOMETRY_MODULAR_INSTANCED );
	BIND_ENUM_CONSTANT( GEOMETRY_PROCEDURAL_UNIQUE );
	BIND_ENUM_CONSTANT( GEOMETRY_MAX );

	ClassDB::bind_method( D_METHOD( "apply", "terrain", "path", "settings" ), &Terrain3DLandformBaker::apply );
	ClassDB::bind_method( D_METHOD( "restore", "terrain", "snapshots", "edited_area" ), &Terrain3DLandformBaker::restore );
	ClassDB::bind_method( D_METHOD( "build_cliff_mesh", "terrain", "path", "settings" ),
						 &Terrain3DLandformBaker::buildCliffMesh );
	ClassDB::bind_method( D_METHOD( "build_cliff_modules", "terrain", "path", "settings" ),
						 &Terrain3DLandformBaker::buildCliffModules );
	ClassDB::bind_method( D_METHOD( "build_cliff_collision_mesh", "terrain", "path", "settings" ),
						 &Terrain3DLandformBaker::buildCliffCollisionMesh );
}
