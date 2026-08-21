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

typedef struct t3dSettings_s {
	t3dMode_t m_mode = RIDGE;
	t3dMorphologyMode_t m_morphologyMode = MORPH_EXPAND_PEAKS;
	float m_fWidth = 32.0f;
	float m_fAmplitude = 12.0f;
	float m_fStrength = 1.0f;
	float m_fFalloff = 0.65f;
	float m_fCrestWidth = 0.18f;
	float m_fAsymmetry = 0.0f;
	float m_fCliffSide = 1.0f;
	float m_fNoise = 0.12f;
	float m_fNoiseScale = 24.0f;
	float m_fTerraceStep = 4.0f;
	float m_fRoadCrown = 0.15f;
	float m_fWaterLevel = 0.0f;
	float m_fShoreSlope = 0.08f;
	float m_fBasinFloorWidth = 0.35f;
	bool m_bBasinCreateRegions = false;
	float m_fDuneSpacing = 28.0f;
	float m_fAngleOfRepose = 36.0f;
	int m_nIterations = 4;
	int m_nSeed = 1337;
	bool m_bAutoRegions = true;
	bool m_bPaintMaterial = false;
	int m_nBaseTexture = 0;
	int m_nOverlayTexture = 0;
	float m_fMaterialBlend = 0.0f;
	bool m_bGenerateMesh = false;
	bool m_bGenerateCollision = true;
	t3dGeometryMode_t m_geometryMode = GEOMETRY_PROCEDURAL_UNIQUE;
	float m_fMeshDepth = 5.0f;
	float m_fMeshIrregularity = 0.28f;
	float m_fMeshSampleSpacing = 3.0f;
	float m_fMeshLip = 1.25f;
	float m_fMeshToe = 1.5f;
	float m_fModuleLength = 8.0f;
	float m_fModuleCornerAngle = 18.0f;
	float m_fModuleMinScale = 0.65f;
	float m_fModuleMaxScale = 1.75f;
	float m_fCollisionSpacing = 8.0f;
	eastl::vector<int> m_moduleMeshIds;
	eastl::vector<int> m_cornerMeshIds;
	eastl::vector<int> m_endMeshIds;
} t3dSettings_t;

typedef struct t3dPathProjection_s {
	float m_fDistance = std::numeric_limits<float>::infinity();
	float m_fSignedDistance = 0.0f;
	float m_fPathDistance = 0.0f;
	float m_fPathHeight = 0.0f;
	Vector2 m_tangent = Vector2( 1.0f, 0.0f );
	Vector2 m_position = V2_ZERO;
} t3dPathProjection_t;

typedef struct t3dBasinProfile_s {
	float m_fPathLength = 0.0f;
	float m_fHeightSlope = 0.0f;
	float m_fHeightIntercept = 0.0f;
	bool m_bValid = false;
} t3dBasinProfile_t;

typedef struct t3dRegionEdit_s {
	Vector2i m_location = V2I_ZERO;
	Ref<T3DRegion> m_region;
	Ref<Image> m_height;
	Ref<Image> m_control;
	eastl::vector<uint8_t> m_heightSnapshot;
	Image::Format m_heightFormat = Image::FORMAT_MAX;
	eastl::vector<float> m_sourceValues;
	eastl::vector<float> m_outputValues;
	bool m_bCalculationValid = true;
	bool m_bExistedBefore = true;
	bool m_bHeightModified = false;
	bool m_bControlModified = false;
} t3dRegionEdit_t;

typedef struct t3dLandformModuleCandidate_s {
	int m_nMeshId = -1;
	float m_fScore = std::numeric_limits<float>::max();
} t3dLandformModuleCandidate_t;

EA_FORCE_INLINE static bool t3dLandformBakerLandformModuleCandidateLess( const t3dLandformModuleCandidate_t &rA, const t3dLandformModuleCandidate_t &rB ) { return rA.m_fScore < rB.m_fScore; }

EA_FORCE_INLINE static float t3dLandformBakerSampleHeightOr( T3DSampler &rSampler, const Vector2 &rPosition, const float fFallback ) { const float fSampled = rSampler.sampleHeight( Vector3( rPosition.x, 0.0f, rPosition.y ) ); return std::isfinite( fSampled ) ? fSampled : fFallback; }

static eastl::vector<int> t3dLandformBakerParseMeshIds( const Variant &rValue ) {
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

static t3dSettings_t t3dLandformBakerParseSettings( const Dictionary &rDictionary ) {
	t3dSettings_t settings;
	settings.m_mode = ( t3dMode_t ) ( CLAMP( ( int ) ( rDictionary.get( "mode", ( int ) ( settings.m_mode ) ) ), 0,
														 ( int ) ( MODE_MAX ) - 1 ) );
	settings.m_morphologyMode = ( t3dMorphologyMode_t ) ( CLAMP( ( int ) ( rDictionary.get( "morphology_mode", ( int ) ( settings.m_morphologyMode ) ) ), 0,
			  ( int ) ( MORPH_MAX ) - 1 ) );
	settings.m_fWidth = MAX( ( float ) ( rDictionary.get( "width", settings.m_fWidth ) ), 1.0f );
	settings.m_fAmplitude = MAX( ( float ) ( rDictionary.get( "amplitude", settings.m_fAmplitude ) ), 0.0f );
	settings.m_fStrength = CLAMP( ( float ) ( rDictionary.get( "strength", settings.m_fStrength ) ), 0.0f, 1.0f );
	settings.m_fFalloff = CLAMP( ( float ) ( rDictionary.get( "falloff", settings.m_fFalloff ) ), 0.01f, 1.0f );
	settings.m_fCrestWidth = CLAMP( ( float ) ( rDictionary.get( "crest_width", settings.m_fCrestWidth ) ), 0.0f, 0.95f );
	settings.m_fAsymmetry = CLAMP( ( float ) ( rDictionary.get( "asymmetry", settings.m_fAsymmetry ) ), -1.0f, 1.0f );
	settings.m_fCliffSide = ( int ) ( rDictionary.get( "cliff_side", 1 ) ) == 0 ? -1.0f : 1.0f;
	settings.m_fNoise = CLAMP( ( float ) ( rDictionary.get( "noise", settings.m_fNoise ) ), 0.0f, 1.0f );
	settings.m_fNoiseScale = MAX( ( float ) ( rDictionary.get( "noise_scale", settings.m_fNoiseScale ) ), 0.25f );
	settings.m_fTerraceStep = MAX( ( float ) ( rDictionary.get( "terrace_step", settings.m_fTerraceStep ) ), 0.05f );
	settings.m_fRoadCrown = ( float ) ( rDictionary.get( "road_crown", settings.m_fRoadCrown ) );
	settings.m_fWaterLevel = ( float ) ( rDictionary.get( "water_level", settings.m_fWaterLevel ) );
	settings.m_fShoreSlope = ( float ) ( rDictionary.get( "shore_slope", settings.m_fShoreSlope ) );
	settings.m_fBasinFloorWidth =
		CLAMP( ( float ) ( rDictionary.get( "basin_floor_width", settings.m_fBasinFloorWidth ) ), 0.0f, 0.95f );
	settings.m_bBasinCreateRegions = ( bool ) ( rDictionary.get( "basin_create_regions", settings.m_bBasinCreateRegions ) );
	settings.m_fDuneSpacing = MAX( ( float ) ( rDictionary.get( "dune_spacing", settings.m_fDuneSpacing ) ), 1.0f );
	settings.m_fAngleOfRepose = CLAMP( ( float ) ( rDictionary.get( "angle_of_repose", settings.m_fAngleOfRepose ) ), 1.0f, 89.0f );
	settings.m_nIterations = CLAMP( ( int ) ( rDictionary.get( "iterations", settings.m_nIterations ) ), 1, 32 );
	settings.m_nSeed = ( int ) ( rDictionary.get( "seed", settings.m_nSeed ) );
	settings.m_bAutoRegions = ( bool ) ( rDictionary.get( "auto_regions", settings.m_bAutoRegions ) );
	settings.m_bPaintMaterial = ( bool ) ( rDictionary.get( "paint_material", settings.m_bPaintMaterial ) );
	settings.m_nBaseTexture = CLAMP( ( int ) ( rDictionary.get( "base_texture", settings.m_nBaseTexture ) ), 0, 31 );
	settings.m_nOverlayTexture = CLAMP( ( int ) ( rDictionary.get( "overlay_texture", settings.m_nOverlayTexture ) ), 0, 31 );
	settings.m_fMaterialBlend = CLAMP( ( float ) ( rDictionary.get( "material_blend", settings.m_fMaterialBlend ) ), 0.0f, 1.0f );
	settings.m_bGenerateMesh = ( bool ) ( rDictionary.get( "generate_mesh", settings.m_bGenerateMesh ) );
	settings.m_bGenerateCollision = ( bool ) ( rDictionary.get( "generate_collision", settings.m_bGenerateCollision ) );
	settings.m_geometryMode = ( t3dGeometryMode_t ) ( CLAMP(
		( int ) ( rDictionary.get( "geometry_mode", ( int ) ( settings.m_geometryMode ) ) ), 0,
		( int ) ( GEOMETRY_MAX ) - 1 ) );
	settings.m_fMeshDepth = MAX( ( float ) ( rDictionary.get( "mesh_depth", settings.m_fMeshDepth ) ), 0.05f );
	settings.m_fMeshIrregularity = CLAMP( ( float ) ( rDictionary.get( "mesh_irregularity", settings.m_fMeshIrregularity ) ), 0.0f, 1.0f );
	settings.m_fMeshSampleSpacing = MAX( ( float ) ( rDictionary.get( "mesh_sample_spacing", settings.m_fMeshSampleSpacing ) ), 0.25f );
	settings.m_fMeshLip = MAX( ( float ) ( rDictionary.get( "mesh_lip", settings.m_fMeshLip ) ), 0.0f );
	settings.m_fMeshToe = MAX( ( float ) ( rDictionary.get( "mesh_toe", settings.m_fMeshToe ) ), 0.0f );
	settings.m_fModuleLength = MAX( ( float ) ( rDictionary.get( "module_length", settings.m_fModuleLength ) ), 0.5f );
	settings.m_fModuleCornerAngle =
		CLAMP( ( float ) ( rDictionary.get( "module_corner_angle", settings.m_fModuleCornerAngle ) ), 0.0f, 180.0f );
	settings.m_fModuleMinScale =
		CLAMP( ( float ) ( rDictionary.get( "module_min_scale", settings.m_fModuleMinScale ) ), 0.05f, 10.0f );
	settings.m_fModuleMaxScale = MAX(
		( float ) ( rDictionary.get( "module_max_scale", settings.m_fModuleMaxScale ) ), settings.m_fModuleMinScale );
	settings.m_fCollisionSpacing = MAX(
		( float ) ( rDictionary.get( "collision_spacing", settings.m_fCollisionSpacing ) ), 0.5f );
	settings.m_moduleMeshIds = t3dLandformBakerParseMeshIds( rDictionary.get( "module_mesh_ids", PackedInt32Array() ) );
	settings.m_cornerMeshIds = t3dLandformBakerParseMeshIds( rDictionary.get( "corner_mesh_ids", PackedInt32Array() ) );
	settings.m_endMeshIds = t3dLandformBakerParseMeshIds( rDictionary.get( "end_mesh_ids", PackedInt32Array() ) );
	return settings;
}

EA_FORCE_INLINE static uint32_t t3dLandformBakerHashU32( uint32_t nValue ) { nValue ^= nValue >> 16u; nValue *= 0x7feb352du; nValue ^= nValue >> 15u; nValue *= 0x846ca68bu; nValue ^= nValue >> 16u; return nValue; }

EA_FORCE_INLINE static float t3dLandformBakerSignedNoise( const int iX, const int iZ, const int nSeed ) { const uint32_t x = ( uint32_t ) ( iX ) * s_nHashGoldenRatio32; const uint32_t z = ( uint32_t ) ( iZ ) * s_nHashMix32; const uint32_t value = t3dLandformBakerHashU32( x ^ z ^ ( uint32_t ) ( nSeed ) ); return ( float ) ( value & 0x00ffffffu ) / ( float ) ( 0x007fffffu ) - 1.0f; }

EA_FORCE_INLINE static float t3dLandformBakerSmooth01( const float fValue ) { const float value = CLAMP( fValue, 0.0f, 1.0f ); return value * value * ( 3.0f - 2.0f * value ); }

static float t3dLandformBakerProfileWeight(
	const float fDistance, const float fHalfWidth, const float fCrestWidth,
	const float fFalloff
) {
	if ( fHalfWidth <= s_fLandformEpsilon || fDistance >= fHalfWidth ) {
		return 0.0f;
	}
	const float fNormalized = fDistance / fHalfWidth;
	if ( fNormalized <= fCrestWidth ) {
		return 1.0f;
	}
	const float fDenominator = MAX( 1.0f - fCrestWidth, s_fLandformEpsilon );
	float fEdge = 1.0f - ( fNormalized - fCrestWidth ) / fDenominator;
	fEdge = Math::pow( CLAMP( fEdge, 0.0f, 1.0f ), Math::lerp( 3.5f, 0.55f, fFalloff ) );
	return t3dLandformBakerSmooth01( fEdge );
}

EA_FORCE_INLINE static float t3dLandformBakerSegmentLengthXz( const Vector3 &vA, const Vector3 &rB ) { return Vector2( rB.x - vA.x, rB.z - vA.z ).length(); }

static t3dPathProjection_t t3dLandformBakerProjectToPath( const PackedVector3Array &arrPath, const Vector2 &rPoint ) {
	t3dPathProjection_t best;
	float fAccumulated = 0.0f;
	for ( int index = 0; index + 1 < arrPath.size(); ++index ) {
		const Vector3 a3 = arrPath[index];
		const Vector3 b3 = arrPath[index + 1];
		const Vector2 a( a3.x, a3.z );
		const Vector2 b( b3.x, b3.z );
		const Vector2 segment = b - a;
		const float length_squared = segment.length_squared();
		if ( length_squared <= s_fLandformEpsilon ) {
			continue;
		}
		const float fT = CLAMP( ( rPoint - a ).dot( segment ) / length_squared, 0.0f, 1.0f );
		const Vector2 position = a + segment * fT;
		const Vector2 delta = rPoint - position;
		const float fDistance = delta.length();
		if ( fDistance < best.m_fDistance ) {
			const float length = Math::sqrt( length_squared );
			const Vector2 tangent = segment / length;
			best.m_fDistance = fDistance;
			best.m_fSignedDistance = tangent.cross( delta );
			best.m_fPathDistance = fAccumulated + length * fT;
			best.m_fPathHeight = Math::lerp( a3.y, b3.y, fT );
			best.m_tangent = tangent;
			best.m_position = position;
		}
		fAccumulated += Math::sqrt( length_squared );
	}
	if ( !std::isfinite( best.m_fDistance ) && !arrPath.is_empty() ) {
		const Vector3 point = arrPath[0];
		best.m_position = Vector2( point.x, point.z );
		best.m_fDistance = rPoint.distance_to( best.m_position );
		best.m_fPathHeight = point.y;
	}
	return best;
}

static t3dBasinProfile_t t3dLandformBakerBuildBasinProfile( const PackedVector3Array &arrPath ) {
	t3dBasinProfile_t profile;
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
			fCumulative += ( double ) ( t3dLandformBakerSegmentLengthXz( arrPath[index - 1], arrPath[index] ) );
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
	if ( Math::abs( fDenominator ) > ( double ) ( s_fLandformEpsilon ) ) {
		fSlope = ( ( double ) ( sample_count ) * fSumDistanceHeight - fSumDistance * fSumHeight ) / fDenominator;
	}
	const double fIntercept = ( fSumHeight - fSlope * fSumDistance ) / ( double ) ( sample_count );
	profile.m_fPathLength = ( float ) ( MAX( fCumulative, 0.0 ) );
	profile.m_fHeightSlope = ( float ) ( fSlope );
	profile.m_fHeightIntercept = ( float ) ( fIntercept );
	profile.m_bValid = std::isfinite( profile.m_fHeightSlope ) && std::isfinite( profile.m_fHeightIntercept );
	return profile;
}

static float t3dLandformBakerSampleBasinRimHeight( const t3dBasinProfile_t &rProfile, const t3dPathProjection_t &rProjection ) {
	if ( !rProfile.m_bValid ) {
		return rProjection.m_fPathHeight;
	}
	const float fDistance = CLAMP( rProjection.m_fPathDistance, 0.0f, rProfile.m_fPathLength );
	return rProfile.m_fHeightIntercept + rProfile.m_fHeightSlope * fDistance;
}

static AABB t3dLandformBakerCalculateBounds( const PackedVector3Array &arrPath, const t3dSettings_t &rSettings, const float fSpacing ) {
	Vector3 minimum(
		std::numeric_limits<real_t>::infinity(), s_fRegionHeightMin,
		std::numeric_limits<real_t>::infinity()
	);
	Vector3 maximum(
		-std::numeric_limits<real_t>::infinity(), s_fRegionHeightMax,
		-std::numeric_limits<real_t>::infinity()
	);
	for ( int index = 0; index < arrPath.size(); ++index ) {
		const Vector3 point = arrPath[index];
		minimum.x = MIN( minimum.x, point.x );
		minimum.z = MIN( minimum.z, point.z );
		maximum.x = MAX( maximum.x, point.x );
		maximum.z = MAX( maximum.z, point.z );
	}
	const float fRadius = MAX( rSettings.m_fWidth * 0.5f, fSpacing );
	minimum.x -= fRadius + fSpacing;
	minimum.z -= fRadius + fSpacing;
	maximum.x += fRadius + fSpacing;
	maximum.z += fRadius + fSpacing;
	return AABB( minimum, maximum - minimum );
}

static Dictionary t3dLandformBakerCaptureSnapshot( const t3dRegionEdit_t &rEdit, const bool bExisted ) {
	Dictionary snapshot;
	snapshot["location"] = rEdit.m_location;
	snapshot["exists"] = bExisted;
	if ( !bExisted || rEdit.m_region.is_null() ) {
		return snapshot;
	}
	const Ref<Image> pHeight = rEdit.m_region->getHeightMap();
	const Ref<Image> pControl = rEdit.m_region->getControlMap();
	snapshot["height"] = pHeight.is_valid() ? Variant( pHeight->duplicate() ) : Variant();
	snapshot["control"] = pControl.is_valid() ? Variant( pControl->duplicate() ) : Variant();
	return snapshot;
}

static bool t3dLandformBakerValidateHeightMap( const Image *pHeight, const int nRegionSize ) {
	if ( !pHeight || pHeight->get_width() != nRegionSize || pHeight->get_height() != nRegionSize ) {
		return false;
	}
	const Image::Format format = pHeight->get_format();
	if ( format != Image::FORMAT_R16 && format != Image::FORMAT_RF ) {
		return false;
	}
	const size_t expected_bytes = t3dRegionCodecExpectedHeightByteCount( nRegionSize, format );
	return expected_bytes > 0u && ( size_t ) ( pHeight->get_data().size() ) == expected_bytes;
}

static bool t3dLandformBakerValidateControlMap( const Ref<Image> &rControl, const int nRegionSize ) {
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

static void t3dLandformBakerRemoveCreatedRegions( T3DData *pData, const eastl::vector<t3dRegionEdit_t> &rEdits ) {
	if ( !pData ) {
		return;
	}
	for ( const t3dRegionEdit_t &edit : rEdits ) {
		if ( !edit.m_bExistedBefore && pData->hasRegion( edit.m_location ) ) {
			pData->removeRegionl( edit.m_location, true );
		}
	}
}

static uint32_t t3dLandformBakerUpdateMaterial( const uint32_t nSource, const t3dSettings_t &rSettings ) {
	const uint8_t blend = ( uint8_t ) ( CLAMP( Math::round( rSettings.m_fMaterialBlend * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
	return ( nSource & ~s_nLandformMaterialMask ) | encBase( ( uint8_t ) ( rSettings.m_nBaseTexture ) ) |
								 encOverlay( ( uint8_t ) ( rSettings.m_nOverlayTexture ) ) | encBlend( blend ) | encAuto( false );
}

static float t3dLandformBakerNeighborValue(
	const float *const pValues, const int nSize, const int iX, const int iY,
	const float fFallback
) {
	if ( iX < 0 || iY < 0 || iX >= nSize || iY >= nSize ) {
		return fFallback;
	}
	return pValues[( size_t ) ( iY * nSize + iX )];
}

static float t3dLandformBakerMorphologyTarget(
	const float *const pValues, const int nSize, const int iX, const int iY,
	const t3dMorphologyMode_t mode
) {
	const float fSource = pValues[( size_t ) ( iY * nSize + iX )];
	float fMinimum = fSource;
	float fMaximum = fSource;
	float fSum = 0.0f;
	int count = 0;
	for ( int offset_y = -1; offset_y <= 1; ++offset_y ) {
		for ( int offset_x = -1; offset_x <= 1; ++offset_x ) {
			const float fValue = t3dLandformBakerNeighborValue( pValues, nSize, iX + offset_x, iY + offset_y, fSource );
			fMinimum = MIN( fMinimum, fValue );
			fMaximum = MAX( fMaximum, fValue );
			fSum += fValue;
			++count;
		}
	}
	const float fAverage = fSum / ( float ) ( count );
	switch ( mode ) {
		case MORPH_FILL_PITS:
			return fMaximum;
		case MORPH_RIDGE_SHARPEN:
			return fSource + MAX( fSource - fAverage, 0.0f );
		case MORPH_VALLEY_SHARPEN:
			return fSource - MAX( fAverage - fSource, 0.0f );
		case MORPH_PLATEAU_CLEAN:
			return Math::abs( fSource - fAverage ) < 2.0f ? fAverage : fSource;
		case MORPH_EXPAND_PEAKS:
		default:
			return fMaximum;
	}
}

static float t3dLandformBakerThermalTarget(
	const float *const pValues, const int nSize, const int iX, const int iY,
	const float fSpacing, const float fAngleDegrees
) {
	const float fSource = pValues[( size_t ) ( iY * nSize + iX )];
	const float fMaximumDelta = Math::tan( Math::deg_to_rad( fAngleDegrees ) ) * fSpacing;
	float fSum = 0.0f;
	int count = 0;
	for ( int offset_y = -1; offset_y <= 1; ++offset_y ) {
		for ( int offset_x = -1; offset_x <= 1; ++offset_x ) {
			if ( offset_x == 0 && offset_y == 0 ) {
				continue;
			}
			const float fNeighbor = t3dLandformBakerNeighborValue( pValues, nSize, iX + offset_x, iY + offset_y, fSource );
			const float fDelta = fNeighbor - fSource;
			if ( Math::abs( fDelta ) > fMaximumDelta ) {
				fSum += fNeighbor - Math::sign( fDelta ) * fMaximumDelta;
				++count;
			}
		}
	}
	return count > 0 ? fSum / ( float ) ( count ) : fSource;
}

typedef enum t3dHeightPolicy_e {
	FREE,
	RAISE_ONLY,
	LOWER_ONLY,
} t3dHeightPolicy_t;

static t3dHeightPolicy_t t3dLandformBakerGetHeightPolicy( const t3dSettings_t &rSettings ) {
	switch ( rSettings.m_mode ) {
		case RIDGE:
		case SCARP:
		case PLATEAU:
		case TALUS:
		case DUNE:
		case OCCLUSION:
			return t3dHeightPolicy_t::RAISE_ONLY;
		case VALLEY:
		case BASIN:
		case PASS:
			return t3dHeightPolicy_t::LOWER_ONLY;
		case MORPHOLOGY:
			switch ( rSettings.m_morphologyMode ) {
				case MORPH_EXPAND_PEAKS:
				case MORPH_FILL_PITS:
				case MORPH_RIDGE_SHARPEN:
					return t3dHeightPolicy_t::RAISE_ONLY;
				case MORPH_VALLEY_SHARPEN:
					return t3dHeightPolicy_t::LOWER_ONLY;
				case MORPH_PLATEAU_CLEAN:
				default:
					return t3dHeightPolicy_t::FREE;
			}
		default:
			return t3dHeightPolicy_t::FREE;
	}
}

static float t3dLandformBakerEnforceHeightPolicy( const t3dSettings_t &rSettings, const float fSource, const float fTarget ) {
	const float fBlended = Math::lerp( fSource, fTarget, rSettings.m_fStrength );
	switch ( t3dLandformBakerGetHeightPolicy( rSettings ) ) {
		case t3dHeightPolicy_t::RAISE_ONLY:
			return MAX( fSource, fBlended );
		case t3dHeightPolicy_t::LOWER_ONLY:
			return MIN( fSource, fBlended );
		case t3dHeightPolicy_t::FREE:
		default:
			return fBlended;
	}
}

static float t3dLandformBakerEvaluateHeight(
	const t3dSettings_t &rSettings, const t3dPathProjection_t &rProjection, const Vector2 &rWorld,
	const Vector2 &rAreaCenter, const float fAreaRadius, const float fSource,
	const float fNeighborTarget, const t3dBasinProfile_t &rBasinProfile
) {
	const float fHalfWidth = MAX( rSettings.m_fWidth * 0.5f, 0.5f );
	const float fWeight = t3dLandformBakerProfileWeight( rProjection.m_fDistance, fHalfWidth, rSettings.m_fCrestWidth, rSettings.m_fFalloff );
	const float fGridNoise = t3dLandformBakerSignedNoise( ( int ) ( Math::floor( rWorld.x / rSettings.m_fNoiseScale ) ),
										 ( int ) ( Math::floor( rWorld.y / rSettings.m_fNoiseScale ) ), rSettings.m_nSeed );
	const float fNoiseFactor = 1.0f + fGridNoise * rSettings.m_fNoise;
	float fTarget = fSource;

	switch ( rSettings.m_mode ) {
		case RIDGE:
			fTarget = fSource + rSettings.m_fAmplitude * fWeight * fNoiseFactor;
			break;
		case VALLEY:
			fTarget = fSource - rSettings.m_fAmplitude * fWeight * fNoiseFactor;
			break;
		case SCARP: {
			const float fTransition = MAX( 0.025f, 0.18f * ( 1.0f - Math::abs( rSettings.m_fAsymmetry ) * 0.75f ) );
			const float fRaisedDistance = rProjection.m_fSignedDistance * rSettings.m_fCliffSide;
			const float fSide = CLAMP( fRaisedDistance / fHalfWidth - rSettings.m_fAsymmetry * 0.25f, -1.0f, 1.0f );
			const float fStep = t3dLandformBakerSmooth01( ( fSide + fTransition ) / ( fTransition * 2.0f ) );
			fTarget = fSource + fStep * rSettings.m_fAmplitude * fWeight * fNoiseFactor;
			break;
		}
		case BASIN: {
			// The stroke is the basin spine. Its width creates a rounded capsule,
			// while the fitted longitudinal grade removes local cursor-height noise.
			// This produces one continuous floor instead of the old bounding-box
			// circle whose nearest-segment heights could jump across the depression.
			const float fBasinWeight =
				t3dLandformBakerProfileWeight( rProjection.m_fDistance, fHalfWidth, rSettings.m_fBasinFloorWidth, rSettings.m_fFalloff );
			if ( fBasinWeight > 0.0f ) {
				const float fRimHeight = t3dLandformBakerSampleBasinRimHeight( rBasinProfile, rProjection );
				const float fFloorHeight = fRimHeight - rSettings.m_fAmplitude;
				fTarget = Math::lerp( fSource, fFloorHeight, fBasinWeight );
			}
			break;
		}
		case PLATEAU: {
			const float normalized = rWorld.distance_to( rAreaCenter ) / MAX( fAreaRadius, 1.0f );
			if ( normalized <= 1.0f ) {
				const float fEdge = 1.0f - t3dLandformBakerSmooth01( MAX( 0.0f, normalized - rSettings.m_fCrestWidth ) /
												 MAX( 1.0f - rSettings.m_fCrestWidth, s_fLandformEpsilon ) );
				fTarget = Math::lerp( fSource, rProjection.m_fPathHeight + rSettings.m_fAmplitude, fEdge );
			}
			break;
		}
		case TERRACE: {
			const float fBase = rProjection.m_fPathHeight;
			const float fQuantized = fBase + Math::round( ( fSource - fBase ) / rSettings.m_fTerraceStep ) * rSettings.m_fTerraceStep;
			fTarget = Math::lerp( fSource, fQuantized, fWeight );
			break;
		}
		case ROAD: {
			const float fLateral = CLAMP( Math::abs( rProjection.m_fSignedDistance ) / fHalfWidth, 0.0f, 1.0f );
			const float fCrown = rSettings.m_fRoadCrown * ( 1.0f - fLateral * fLateral );
			fTarget = Math::lerp( fSource, rProjection.m_fPathHeight + fCrown, fWeight );
			break;
		}
		case PASS:
			fTarget = Math::lerp( fSource, rProjection.m_fPathHeight - rSettings.m_fAmplitude, fWeight );
			break;
		case TALUS: {
			const float fPreferredSide = rSettings.m_fAsymmetry < 0.0f ? -1.0f : 1.0f;
			const float fSide = MAX( 0.0f, rProjection.m_fSignedDistance * fPreferredSide / fHalfWidth );
			const float fApron = ( 1.0f - CLAMP( fSide, 0.0f, 1.0f ) ) * fWeight;
			fTarget = fSource + rSettings.m_fAmplitude * fApron * ( 0.7f + 0.3f * fNoiseFactor );
			break;
		}
		case HYDRAULIC_CHANNEL: {
			const float normalized = rProjection.m_fDistance / fHalfWidth;
			const float fChannel = Math::exp( -normalized * normalized * 10.0f );
			const float fBank = Math::exp( -Math::pow( normalized - 0.62f, 2.0f ) * 38.0f );
			fTarget = fSource - rSettings.m_fAmplitude * fChannel * fNoiseFactor + rSettings.m_fAmplitude * 0.18f * fBank;
			break;
		}
		case THERMAL_RELAXATION:
			fTarget = Math::lerp( fSource, fNeighborTarget, fWeight );
			break;
		case DUNE: {
			const Vector2 wind = rProjection.m_tangent.length_squared() > s_fLandformEpsilon ? rProjection.m_tangent.normalized()
																				 : Vector2( 1.0f, 0.0f );
			float fPhase = Math::fmod( rWorld.dot( wind ) / rSettings.m_fDuneSpacing, 1.0f );
			if ( fPhase < 0.0f ) {
				fPhase += 1.0f;
			}
			const float fDune = fPhase < 0.78f ? fPhase / 0.78f : ( 1.0f - fPhase ) / 0.22f;
			fTarget = fSource + rSettings.m_fAmplitude * t3dLandformBakerSmooth01( fDune ) * fWeight * fNoiseFactor;
			break;
		}
		case SHORELINE: {
			const float fDesired = rSettings.m_fWaterLevel + rProjection.m_fSignedDistance * rSettings.m_fShoreSlope;
			fTarget = Math::lerp( fSource, fDesired, fWeight );
			break;
		}
		case MORPHOLOGY:
			fTarget = Math::lerp( fSource, fNeighborTarget, fWeight );
			break;
		case OCCLUSION: {
			const float fRidge = rSettings.m_fAmplitude * fWeight * fNoiseFactor;
			const float fSide = CLAMP( rProjection.m_fSignedDistance / fHalfWidth, -1.0f, 1.0f );
			fTarget = fSource + fRidge * ( 1.0f - 0.28f * MAX( fSide, 0.0f ) );
			break;
		}
		default:
			break;
	}
	return t3dLandformBakerEnforceHeightPolicy( rSettings, fSource, fTarget );
}

static PackedVector3Array t3dLandformBakerSmoothPath( const PackedVector3Array &arrPath, const int nPasses ) {
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

static PackedVector3Array t3dLandformBakerResamplePath( const PackedVector3Array &arrPath, const float fSpacing ) {
	PackedVector3Array result;
	if ( arrPath.is_empty() ) {
		return result;
	}
	result.push_back( arrPath[0] );
	for ( int index = 0; index + 1 < arrPath.size(); ++index ) {
		const Vector3 a = arrPath[index];
		const Vector3 b = arrPath[index + 1];
		const float fLength = t3dLandformBakerSegmentLengthXz( a, b );
		if ( fLength <= s_fLandformEpsilon ) {
			continue;
		}
		const int steps = MAX( 1, ( int ) ( Math::ceil( fLength / fSpacing ) ) );
		for ( int step = 1; step <= steps; ++step ) {
			result.push_back( a.lerp( b, ( float ) ( step ) / ( float ) ( steps ) ) );
		}
	}
	return result;
}

EA_FORCE_INLINE static void t3dLandformBakerAddTriangle( const Ref<SurfaceTool> &rSurface, const Vector3 &vA, const Vector3 &rB, const Vector3 &rC, const Vector2 &rUvA, const Vector2 &rUvB, const Vector2 &rUvC ) { rSurface->set_uv( rUvA ); rSurface->add_vertex( vA ); rSurface->set_uv( rUvB ); rSurface->add_vertex( rB ); rSurface->set_uv( rUvC ); rSurface->add_vertex( rC ); }

EA_FORCE_INLINE static void t3dLandformBakerAddQuad(
	const Ref<SurfaceTool> &rSurface, const Vector3 &rATop, const Vector3 &rABottom,
	const Vector3 &rBBottom, const Vector3 &rBTop, const Vector2 &rUvATop,
	const Vector2 &rUvABottom, const Vector2 &rUvBBottom, const Vector2 &rUvBTop
) { t3dLandformBakerAddTriangle( rSurface, rATop, rABottom, rBBottom, rUvATop, rUvABottom, rUvBBottom ); t3dLandformBakerAddTriangle( rSurface, rATop, rBBottom, rBTop, rUvATop, rUvBBottom, rUvBTop ); }

static Ref<StandardMaterial3D> t3dLandformBakerCreateCliffMaterial(
	T3DTerrain *pTerrain, const t3dSettings_t &rSettings,
	Dictionary &rCache
) {
	String cache_key = "default";
	Ref<T3DTextureAsset> pAsset;
	if ( pTerrain && pTerrain->getAssets().is_valid() ) {
		pAsset = pTerrain->getAssets()->getTextureAsset( rSettings.m_nBaseTexture );
		cache_key = String::num_uint64( ( uint64_t ) ( pTerrain->get_instance_id() ) ) + ":" +
			String::num_int64( rSettings.m_nBaseTexture ) + ":" +
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

static eastl::vector<int> t3dLandformBakerGetValidMeshIds( T3DTerrain *pTerrain, const eastl::vector<int> &nIds ) {
	eastl::vector<int> valid;
	if ( !pTerrain || pTerrain->getAssets().is_null() ) {
		return valid;
	}
	valid.reserve( nIds.size() );
	for ( const int mesh_id : nIds ) {
		if ( mesh_id < 0 || mesh_id >= pTerrain->getAssets()->getMeshCount() ) {
			continue;
		}
		const Ref<T3DMeshAsset> pAsset = pTerrain->getAssets()->getMeshAsset( mesh_id );
		if ( pAsset.is_valid() && pAsset->isEnabled() && pAsset->getMesh( 0 ).is_valid() ) {
			valid.push_back( mesh_id );
		}
	}
	return valid;
}

static float t3dLandformBakerModuleScalePenalty( const float fScale, const float fMinScale, const float fMaxScale ) {
	const float fSafeScale = MAX( fScale, s_fLandformEpsilon );
	float fPenalty = Math::abs( Math::log( fSafeScale ) );
	if ( fScale < fMinScale ) {
		fPenalty += 100.0f + ( fMinScale - fScale ) * 100.0f;
	} else if ( fScale > fMaxScale ) {
		fPenalty += 100.0f + ( fScale - fMaxScale ) * 100.0f;
	}
	return fPenalty;
}

static int t3dLandformBakerChooseModuleId(
	T3DTerrain *pTerrain, const eastl::vector<int> &nPool, const float fTargetLength,
	const float fTargetHeight, const float fTargetDepth, const float fMinScale,
	const float fMaxScale, const int iSegmentIndex, const Vector3 &rCenter,
	const int nSeed, const int nPreviousId
) {
	if ( !pTerrain || pTerrain->getAssets().is_null() || nPool.empty() ) {
		return -1;
	}

	eastl::vector<t3dLandformModuleCandidate_t> candidates;
	candidates.reserve( nPool.size() );
	const int world_x = ( int ) ( Math::floor( rCenter.x * 0.25f ) );
	const int world_z = ( int ) ( Math::floor( rCenter.z * 0.25f ) );
	const uint32_t base_hash = t3dLandformBakerHashU32( ( uint32_t ) ( world_x ) ^ ( ( uint32_t ) ( world_z ) * s_nHashGoldenRatio32 ) ^
		( ( uint32_t ) ( iSegmentIndex ) * s_nHashMix32 ) ^ ( uint32_t ) ( nSeed ) );

	for ( const int mesh_id : nPool ) {
		const Ref<T3DMeshAsset> pAsset = pTerrain->getAssets()->getMeshAsset( mesh_id );
		const Ref<Mesh> pSourceMesh = pAsset.is_valid() ? pAsset->getMesh( 0 ) : Ref<Mesh>();
		if ( pSourceMesh.is_null() ) {
			continue;
		}
		const Vector3 size = pSourceMesh->get_aabb().size;
		if ( size.x <= s_fLandformEpsilon || size.y <= s_fLandformEpsilon || size.z <= s_fLandformEpsilon ) {
			continue;
		}
		const float fScaleX = fTargetLength / size.x;
		const float fScaleY = fTargetHeight / size.y;
		const float fScaleZ = fTargetDepth / size.z;
		float score = t3dLandformBakerModuleScalePenalty( fScaleX, fMinScale, fMaxScale );
		score += t3dLandformBakerModuleScalePenalty( fScaleY, fMinScale, fMaxScale ) * 1.5f;
		score += t3dLandformBakerModuleScalePenalty( fScaleZ, fMinScale, fMaxScale ) * 0.5f;
		const uint32_t tie_hash = t3dLandformBakerHashU32( base_hash ^ ( ( uint32_t ) ( mesh_id ) * 0x27d4eb2du ) );
		score += ( float ) ( tie_hash & 0xFFFFu ) / 65535.0f * 0.05f;
		candidates.push_back( {mesh_id, score} );
	}
	if ( candidates.empty() ) {
		return -1;
	}
	std::sort( candidates.begin(), candidates.end(), t3dLandformBakerLandformModuleCandidateLess );

	const float fViableScore = candidates.front().m_fScore + 0.75f;
	size_t viable_count = 1;
	while ( viable_count < candidates.size() && viable_count < 3 && candidates[viable_count].m_fScore <= fViableScore ) {
		++viable_count;
	}
	const size_t offset = ( size_t ) ( base_hash % ( uint32_t ) ( viable_count ) );
	for ( size_t attempt = 0; attempt < viable_count; ++attempt ) {
		const t3dLandformModuleCandidate_t &candidate = candidates[( offset + attempt ) % viable_count];
		if ( viable_count == 1 || candidate.m_nMeshId != nPreviousId ) {
			return candidate.m_nMeshId;
		}
	}
	for ( const t3dLandformModuleCandidate_t &candidate : candidates ) {
		if ( candidate.m_nMeshId != nPreviousId ) {
			return candidate.m_nMeshId;
		}
	}
	return candidates.front().m_nMeshId;
}

static Vector2 t3dLandformBakerSegmentTangent( const PackedVector3Array &rSamples, const int iIndex ) {
	if ( rSamples.size() < 2 ) {
		return Vector2( 1.0f, 0.0f );
	}
	const int a_index = CLAMP( iIndex, 0, rSamples.size() - 2 );
	const Vector3 a = rSamples[a_index];
	const Vector3 b = rSamples[a_index + 1];
	Vector2 tangent( b.x - a.x, b.z - a.z );
	return tangent.length_squared() > s_fLandformEpsilon ? tangent.normalized() : Vector2( 1.0f, 0.0f );
}

typedef struct t3dLandformDecodeContext_s {
	eastl::vector<t3dRegionEdit_t> *m_pEdits = nullptr;
	size_t m_nPixelCount = 0u;
	std::atomic<bool> *m_pInvalidSource = nullptr;
} t3dLandformDecodeContext_t;

static void decodeLandformRegions( void *pContext, const size_t nBegin, const size_t nEnd, int32_t ) {
	t3dLandformDecodeContext_t *context = ( t3dLandformDecodeContext_t * ) ( pContext );
	for ( size_t edit_index = nBegin; edit_index < nEnd; ++edit_index ) {
		t3dRegionEdit_t &edit = ( *context->m_pEdits )[edit_index];
		edit.m_sourceValues.resize( context->m_nPixelCount );
		const uint8_t *pHeightRead = edit.m_heightSnapshot.data();
		for ( size_t index = 0; index < context->m_nPixelCount; ++index ) {
			const float fValue = t3dRegionCodecReadHeightBytes( pHeightRead, ( int ) ( index ), edit.m_heightFormat );
			if ( !std::isfinite( fValue ) ) {
				edit.m_bCalculationValid = false;
				context->m_pInvalidSource->store( true, std::memory_order_relaxed );
				break;
			}
			edit.m_sourceValues[index] = fValue;
		}
		edit.m_heightSnapshot.clear();
	}
}

typedef struct t3dLandformCalculateContext_s {
	eastl::vector<t3dRegionEdit_t> *m_pEdits = nullptr;
	const t3dSettings_t *m_pSettings = nullptr;
	const PackedVector3Array *m_pOperationPath = nullptr;
	const AABB *m_pEditedArea = nullptr;
	const t3dBasinProfile_t *m_pBasinProfile = nullptr;
	Vector2 m_areaCenter = Vector2();
	float m_fAreaRadius = 0.0f;
	float m_fSpacing = 1.0f;
	int m_nRegionSize = 0;
	std::atomic<bool> *m_pInvalidOutput = nullptr;
	float *m_pWorkerScratch = nullptr;
	size_t m_nWorkerScratchStride = 0u;
} t3dLandformCalculateContext_t;

static void calculateLandformRegions( void *pContext, const size_t nBegin, const size_t nEnd, const int32_t nWorker ) {
	t3dLandformCalculateContext_t *context = ( t3dLandformCalculateContext_t * ) ( pContext );
	const t3dSettings_t &settings = *context->m_pSettings;
	const PackedVector3Array &operation_path = *context->m_pOperationPath;
	const AABB &edited_area = *context->m_pEditedArea;
	const t3dBasinProfile_t &basin_profile = *context->m_pBasinProfile;
	float *const pWorkerScratch = context->m_pWorkerScratch + ( size_t ) MAX( nWorker, 0 ) * context->m_nWorkerScratchStride;
	for ( size_t edit_index = nBegin; edit_index < nEnd; ++edit_index ) {
		t3dRegionEdit_t &edit = ( *context->m_pEdits )[edit_index];
		const eastl::vector<float> &source_values = edit.m_sourceValues;
		edit.m_outputValues.resize( source_values.size() );
		memcpy( edit.m_outputValues.data(), source_values.data(), source_values.size() * sizeof( float ) );
		float *pWorking = edit.m_outputValues.data();
		float *pNext = pWorkerScratch;
		const int relaxation_passes = settings.m_mode == THERMAL_RELAXATION ? settings.m_nIterations : 1;
		for ( int pass = 0; pass < relaxation_passes; ++pass ) {
			memcpy( pNext, pWorking, source_values.size() * sizeof( float ) );
			for ( int y = 0; y < context->m_nRegionSize; ++y ) {
				for ( int x = 0; x < context->m_nRegionSize; ++x ) {
					const float fWorldX = ( float ) ( edit.m_location.x * context->m_nRegionSize + x ) * context->m_fSpacing;
					const float fWorldZ = ( float ) ( edit.m_location.y * context->m_nRegionSize + y ) * context->m_fSpacing;
					if ( fWorldX < edited_area.position.x || fWorldX > edited_area.get_end().x ||
						fWorldZ < edited_area.position.z || fWorldZ > edited_area.get_end().z ) {
						continue;
					}
					const Vector2 world( fWorldX, fWorldZ );
					const t3dPathProjection_t projection = t3dLandformBakerProjectToPath( operation_path, world );
					const float fSource = pWorking[( size_t ) ( y * context->m_nRegionSize + x )];
					float fNeighborTarget = fSource;
					if ( settings.m_mode == THERMAL_RELAXATION ) {
						fNeighborTarget = t3dLandformBakerThermalTarget( pWorking, context->m_nRegionSize, x, y, context->m_fSpacing, settings.m_fAngleOfRepose );
					} else if ( settings.m_mode == MORPHOLOGY ) {
						fNeighborTarget = t3dLandformBakerMorphologyTarget( pWorking, context->m_nRegionSize, x, y, settings.m_morphologyMode );
					}
					const float fEvaluated = t3dLandformBakerEvaluateHeight( settings, projection, world, context->m_areaCenter,
						context->m_fAreaRadius, fSource, fNeighborTarget, basin_profile );
					if ( !std::isfinite( fEvaluated ) ) {
						edit.m_bCalculationValid = false;
						context->m_pInvalidOutput->store( true, std::memory_order_relaxed );
						continue;
					}
					pNext[( size_t ) ( y * context->m_nRegionSize + x )] =
						CLAMP( fEvaluated, s_fRegionHeightMin, s_fRegionHeightMax );
				}
			}
			std::swap( pWorking, pNext );
		}
		if ( pWorking != edit.m_outputValues.data() ) memcpy( edit.m_outputValues.data(), pWorking, source_values.size() * sizeof( float ) );

		for ( size_t index = 0; index < edit.m_outputValues.size(); ++index ) {
			if ( !Math::is_equal_approx( source_values[index], edit.m_outputValues[index] ) ) {
				edit.m_bHeightModified = true;
				break;
			}
		}
		if ( !edit.m_bCalculationValid ) {
			context->m_pInvalidOutput->store( true, std::memory_order_relaxed );
		}
	}
}

Dictionary T3DLandformBaker::apply(
	T3DTerrain *const pTerrain, const PackedVector3Array &arrPath,
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

	const t3dSettings_t settings = t3dLandformBakerParseSettings( dictSettings );
	const PackedVector3Array operation_path = t3dLandformBakerSmoothPath( arrPath, 2 );
	const t3dBasinProfile_t basin_profile = t3dLandformBakerBuildBasinProfile( operation_path );
	T3DData *const data = pTerrain->getData();
	const int region_size = pTerrain->getRegionSize();
	const float spacing = pTerrain->getVertexSpacing();
	const float fWorldRegionSize = ( float ) ( region_size ) * spacing;
	const AABB edited_area = t3dLandformBakerCalculateBounds( operation_path, settings, spacing );
	const Vector2 areaCenter(
		( edited_area.position.x + edited_area.get_end().x ) * 0.5f,
		( edited_area.position.z + edited_area.get_end().z ) * 0.5f
	);
	const float area_radius = MAX( Vector2( edited_area.size.x, edited_area.size.z ).length() * 0.25f,
							settings.m_fWidth * 0.5f );

	const int min_region_x = ( int ) ( Math::floor( edited_area.position.x / fWorldRegionSize ) );
	const int max_region_x = ( int ) ( Math::floor( ( edited_area.get_end().x - s_fLandformEpsilon ) / fWorldRegionSize ) );
	const int min_region_z = ( int ) ( Math::floor( edited_area.position.z / fWorldRegionSize ) );
	const int max_region_z = ( int ) ( Math::floor( ( edited_area.get_end().z - s_fLandformEpsilon ) / fWorldRegionSize ) );
	eastl::vector<t3dRegionEdit_t> edits;
	bool bAddedRegions = false;
	const bool bAllowRegionCreation =
		settings.m_bAutoRegions && ( settings.m_mode != BASIN || settings.m_bBasinCreateRegions );
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
				t3dLandformBakerRemoveCreatedRegions( data, edits );
				result["error"] = "A terrain region could not be paged into the CPU map cache; no terrain was changed.";
				return result;
			}
			const Ref<T3DRegion> region = data->getRegion( location );
			if ( region.is_null() || region->isDeleted() ) {
				continue;
			}
			t3dRegionEdit_t edit;
			edit.m_location = location;
			edit.m_region = region;
			edit.m_bExistedBefore = bExisted;
			edit.m_height = region->getHeightMap();
			if ( settings.m_bPaintMaterial ) {
				edit.m_control = region->getControlMap();
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
		data->updateMaps( REGION_TYPE_MAX, true, false );
	}

	// Snapshot engine-owned image bytes on the calling thread. Workers only touch
	// plain vectors, so the transactional calculation phase is free of Image and
	// resource access.
	const size_t pixel_count = ( size_t ) ( region_size ) * ( size_t ) ( region_size );
	for ( t3dRegionEdit_t &edit : edits ) {
		Image *const pHeightMap = edit.m_region->getMapPtrForWrite( TYPE_HEIGHT );
		if ( !t3dLandformBakerValidateHeightMap( pHeightMap, region_size ) ) {
			t3dLandformBakerRemoveCreatedRegions( data, edits );
			result["error"] = "The landform overlapped a region with invalid height-map storage; no terrain was changed.";
			return result;
		}
		if ( settings.m_bPaintMaterial && !t3dLandformBakerValidateControlMap( edit.m_region->getControlMap(), region_size ) ) {
			t3dLandformBakerRemoveCreatedRegions( data, edits );
			result["error"] = "The landform overlapped a region with invalid control-map storage; no terrain was changed.";
			return result;
		}
		const PackedByteArray height_bytes = pHeightMap->get_data();
		edit.m_heightSnapshot.resize( ( size_t ) ( height_bytes.size() ) );
		if ( !edit.m_heightSnapshot.empty() ) {
			std::memcpy( edit.m_heightSnapshot.data(), height_bytes.ptr(), edit.m_heightSnapshot.size() );
		}
		edit.m_heightFormat = pHeightMap->get_format();
	}

	const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	const T3DParallelExecutor executor( ( int32_t ) ( MIN<size_t>( ( size_t ) ( hardware_threads ), edits.size() ) ) );
	std::atomic<bool> invalid_source{false};
	t3dLandformDecodeContext_t decode_context;
	decode_context.m_pEdits = &edits;
	decode_context.m_nPixelCount = pixel_count;
	decode_context.m_pInvalidSource = &invalid_source;
	executor.runStatic( edits.size(), 1u, decodeLandformRegions, &decode_context );
	if ( invalid_source.load( std::memory_order_relaxed ) ) {
		t3dLandformBakerRemoveCreatedRegions( data, edits );
		result["error"] = "The landform overlapped non-finite height data; no terrain was changed.";
		return result;
	}

	eastl::vector<Dictionary> before_snapshots;
	before_snapshots.reserve( edits.size() );
	for ( const t3dRegionEdit_t &edit : edits ) {
		before_snapshots.push_back( t3dLandformBakerCaptureSnapshot( edit, edit.m_bExistedBefore ) );
	}

	eastl::vector<Vector2i> height_locations;
	eastl::vector<Vector2i> control_locations;
	std::atomic<bool> invalid_output{false};
	const size_t calculate_worker_count = MIN<size_t>( ( size_t ) ( hardware_threads ), edits.size() );
	eastl::vector<float> calculate_scratch;
	calculate_scratch.resize( calculate_worker_count * pixel_count );
	t3dLandformCalculateContext_t calculate_context;
	calculate_context.m_pEdits = &edits;
	calculate_context.m_pSettings = &settings;
	calculate_context.m_pOperationPath = &operation_path;
	calculate_context.m_pEditedArea = &edited_area;
	calculate_context.m_pBasinProfile = &basin_profile;
	calculate_context.m_areaCenter = areaCenter;
	calculate_context.m_fAreaRadius = area_radius;
	calculate_context.m_fSpacing = spacing;
	calculate_context.m_nRegionSize = region_size;
	calculate_context.m_pInvalidOutput = &invalid_output;
	calculate_context.m_pWorkerScratch = calculate_scratch.data();
	calculate_context.m_nWorkerScratchStride = pixel_count;
	executor.runStatic( edits.size(), 1u, calculateLandformRegions, &calculate_context );
	if ( invalid_output.load( std::memory_order_relaxed ) ) {
		t3dLandformBakerRemoveCreatedRegions( data, edits );
		result["error"] = "The landform produced a non-finite height; no terrain was changed.";
		return result;
	}
	for ( const t3dRegionEdit_t &edit : edits ) {
		if ( edit.m_bHeightModified ) {
			height_locations.push_back( edit.m_location );
		}
	}

	if ( height_locations.empty() ) {
		for ( const t3dRegionEdit_t &edit : edits ) {
			if ( !edit.m_bExistedBefore ) {
				data->removeRegionl( edit.m_location, true );
			}
		}
		result["error"] = "The selected operation produced no terrain changes.";
		return result;
	}

	// Do not retain blank regions that were touched only by the operation bounds.
	// They must also stay out of the undo snapshot, otherwise redo would recreate
	// terrain that the operation never modified.
	for ( const t3dRegionEdit_t &edit : edits ) {
		if ( !edit.m_bExistedBefore && !edit.m_bHeightModified && !edit.m_bControlModified ) {
			data->removeRegionl( edit.m_location, true );
		}
	}

	// Publish only after every region has produced a finite output. This keeps the
	// operation transactional: a failure cannot leave half of a multi-region basin
	// written into the terrain data.
	for ( t3dRegionEdit_t &edit : edits ) {
		if ( !edit.m_bHeightModified ) {
			continue;
		}
		Image *const pHeightMap = edit.m_region->getMapPtrForWrite( TYPE_HEIGHT );
		Image *const pControlMap = settings.m_bPaintMaterial ? edit.m_region->getMapPtrForWrite( TYPE_CONTROL ) : nullptr;
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
			const float fSource = edit.m_sourceValues[index];
			const float fOutput = edit.m_outputValues[index];
			if ( Math::is_equal_approx( fSource, fOutput ) ) {
				continue;
			}
			if ( height_format == Image::FORMAT_R16 ) {
				const uint16_t encoded = t3dRegionCodecEncodeHeightU16( fOutput );
				std::memcpy( pHeightWrite + index * sizeof( uint16_t ), &encoded, sizeof( uint16_t ) );
			} else {
				std::memcpy( pHeightWrite + index * sizeof( float ), &fOutput, sizeof( float ) );
			}
			if ( pControlWrite ) {
				uint32_t source_control = s_nRegionDefaultControl;
				std::memcpy( &source_control, pControlWrite + index * sizeof( uint32_t ), sizeof( uint32_t ) );
				const uint32_t replacement = t3dLandformBakerUpdateMaterial( source_control, settings );
				if ( replacement != source_control ) {
					std::memcpy( pControlWrite + index * sizeof( uint32_t ), &replacement, sizeof( uint32_t ) );
					edit.m_bControlModified = true;
				}
			}
		}
		pHeightMap->set_data( region_size, region_size, pHeightMap->has_mipmaps(), height_format, height_bytes );
		if ( edit.m_bControlModified && pControlMap ) {
			pControlMap->set_data( region_size, region_size, pControlMap->has_mipmaps(), Image::FORMAT_RF, control_bytes );
		}
		if ( edit.m_bControlModified ) {
			control_locations.push_back( edit.m_location );
		}
	}

	uint32_t commit_flags = MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_SNAP;
	if ( pTerrain->getCollisionMode() == DYNAMIC_EDITOR ) {
		commit_flags |= MAP_EDIT_UPDATE_COLLISION;
	}
	data->commitMapEdits( TYPE_HEIGHT, height_locations, edited_area, commit_flags );
	if ( !control_locations.empty() ) {
		data->commitMapEdits( TYPE_CONTROL, control_locations, edited_area, MAP_EDIT_SNAP );
	}

	Array before;
	Array after;
	for ( size_t index = 0; index < edits.size(); ++index ) {
		const t3dRegionEdit_t &edit = edits[index];
		if ( !edit.m_bHeightModified && !edit.m_bControlModified ) {
			continue;
		}
		before.push_back( before_snapshots[index] );
		after.push_back( t3dLandformBakerCaptureSnapshot( edit, true ) );
	}

	result["ok"] = true;
	result["before"] = before;
	result["after"] = after;
	result["edited_area"] = edited_area;
	result["modified_regions"] = ( int ) ( height_locations.size() );
	if ( settings.m_bGenerateMesh && ( settings.m_mode == SCARP || settings.m_mode == OCCLUSION ) ) {
		if ( settings.m_geometryMode == GEOMETRY_MODULAR_INSTANCED ) {
			result["modules"] = buildCliffModules( pTerrain, operation_path, dictSettings );
		} else if ( settings.m_geometryMode == GEOMETRY_PROCEDURAL_UNIQUE ) {
			result["mesh"] = buildCliffMesh( pTerrain, operation_path, dictSettings );
		}
		if ( settings.m_bGenerateCollision && settings.m_geometryMode != GEOMETRY_TERRAIN_ONLY ) {
			result["collision_mesh"] = buildCliffCollisionMesh( pTerrain, operation_path, dictSettings );
		}
	}
	return result;
}

void T3DLandformBaker::restore( T3DTerrain *const pTerrain, const Array &rSnapshots, const AABB &rEditedArea ) {
	if ( !pTerrain || !pTerrain->getData() ) {
		return;
	}
	T3DData *const pData = pTerrain->getData();
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
		Ref<T3DRegion> pRegion = pData->getRegion( location );
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
		pData->updateMaps( REGION_TYPE_MAX, true, false );
	}
	if ( !height_locations.empty() ) {
		uint32_t flags = MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_SNAP;
		if ( pTerrain->getCollisionMode() == DYNAMIC_EDITOR ) {
			flags |= MAP_EDIT_UPDATE_COLLISION;
		}
		pData->commitMapEdits( TYPE_HEIGHT, height_locations, rEditedArea, flags );
	}
	if ( !control_locations.empty() ) {
		pData->commitMapEdits( TYPE_CONTROL, control_locations, rEditedArea, MAP_EDIT_SNAP );
	}
}

Dictionary T3DLandformBaker::buildCliffModules(
	T3DTerrain *const pTerrain,
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

	const t3dSettings_t settings = t3dLandformBakerParseSettings( dictSettings );
	const eastl::vector<int> face_ids = t3dLandformBakerGetValidMeshIds( pTerrain, settings.m_moduleMeshIds );
	const eastl::vector<int> corner_ids = t3dLandformBakerGetValidMeshIds( pTerrain, settings.m_cornerMeshIds );
	const eastl::vector<int> end_ids = t3dLandformBakerGetValidMeshIds( pTerrain, settings.m_endMeshIds );
	if ( face_ids.empty() ) {
		result["error"] = "Modular cliff mode requires at least one valid face mesh asset ID.";
		return result;
	}

	const PackedVector3Array samples = t3dLandformBakerResamplePath( t3dLandformBakerSmoothPath( arrPath, 2 ), settings.m_fModuleLength );
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
		const float fHorizontalLength = t3dLandformBakerSegmentLengthXz( a, b );
		if ( fHorizontalLength <= s_fLandformEpsilon ) {
			continue;
		}
		const Vector3 center = a.lerp( b, 0.5f );
		const Vector2 tangent = t3dLandformBakerSegmentTangent( samples, segment_index );
		const Vector2 previous_tangent = t3dLandformBakerSegmentTangent( samples, MAX( segment_index - 1, 0 ) );
		const Vector2 next_tangent = t3dLandformBakerSegmentTangent( samples, MIN( segment_index + 1, segment_count - 1 ) );
		const float fPreviousTurn = Math::rad_to_deg(
			Math::acos( CLAMP( previous_tangent.dot( tangent ), -1.0f, 1.0f ) ) );
		const float fNextTurn = Math::rad_to_deg( Math::acos( CLAMP( tangent.dot( next_tangent ), -1.0f, 1.0f ) ) );
		const float fTurnAngle = MAX( fPreviousTurn, fNextTurn );
		const bool bIsEnd = segment_index == 0 || segment_index == segment_count - 1;
		const bool bIsCorner = fTurnAngle >= settings.m_fModuleCornerAngle;
		const eastl::vector<int> *pool = &face_ids;
		if ( bIsEnd && !end_ids.empty() ) {
			pool = &end_ids;
		} else if ( bIsCorner && !corner_ids.empty() ) {
			pool = &corner_ids;
		}

		const Vector2 raised_normal = Vector2( -tangent.y, tangent.x ) * settings.m_fCliffSide;
		const float fTransitionOffset = settings.m_fAsymmetry * settings.m_fWidth * 0.125f;
		const Vector2 face_a = Vector2( a.x, a.z ) + raised_normal * fTransitionOffset;
		const Vector2 face_b = Vector2( b.x, b.z ) + raised_normal * fTransitionOffset;
		const Vector2 face_center = face_a.lerp( face_b, 0.5f );
		const float fEdgeSample = MAX( fVertexSpacing * 1.5f, settings.m_fWidth * 0.10f );
		const float fHighA = t3dLandformBakerSampleHeightOr(
			sampler, face_a + raised_normal * fEdgeSample, a.y + settings.m_fAmplitude );
		const float fHighB = t3dLandformBakerSampleHeightOr(
			sampler, face_b + raised_normal * fEdgeSample, b.y + settings.m_fAmplitude );
		const float fLowA = t3dLandformBakerSampleHeightOr( sampler, face_a - raised_normal * fEdgeSample, a.y );
		const float fLowB = t3dLandformBakerSampleHeightOr( sampler, face_b - raised_normal * fEdgeSample, b.y );
		const float fBottomA = MIN( fLowA - settings.m_fMeshToe, fHighA - 0.25f );
		const float fBottomB = MIN( fLowB - settings.m_fMeshToe, fHighB - 0.25f );
		const float fFaceHeight = MAX( ( ( fHighA - fBottomA ) + ( fHighB - fBottomB ) ) * 0.5f, 0.25f );
		const float fTargetDepth = MAX( settings.m_fMeshDepth + settings.m_fMeshLip, fVertexSpacing );
		const float fTargetLength = Vector3( b.x - a.x, fBottomB - fBottomA, b.z - a.z ).length();
		const int mesh_id = t3dLandformBakerChooseModuleId( pTerrain, *pool, fTargetLength, fFaceHeight, fTargetDepth,
			settings.m_fModuleMinScale, settings.m_fModuleMaxScale, segment_index, center, settings.m_nSeed,
			previous_mesh_id );
		if ( mesh_id < 0 ) {
			continue;
		}
		previous_mesh_id = mesh_id;

		const Ref<T3DMeshAsset> pAsset = pTerrain->getAssets()->getMeshAsset( mesh_id );
		const Ref<Mesh> pSourceMesh = pAsset.is_valid() ? pAsset->getMesh( 0 ) : Ref<Mesh>();
		if ( pSourceMesh.is_null() ) {
			continue;
		}
		const AABB source_bounds = pSourceMesh->get_aabb();
		if ( source_bounds.size.x <= s_fLandformEpsilon || source_bounds.size.y <= s_fLandformEpsilon || source_bounds.size.z <= s_fLandformEpsilon ) {
			continue;
		}

		const float fScaleX = CLAMP( fTargetLength / source_bounds.size.x,
			settings.m_fModuleMinScale, settings.m_fModuleMaxScale );
		const float fScaleY = CLAMP( fFaceHeight / source_bounds.size.y,
			settings.m_fModuleMinScale, settings.m_fModuleMaxScale );
		const float fScaleZ = CLAMP( fTargetDepth / source_bounds.size.z,
			settings.m_fModuleMinScale, settings.m_fModuleMaxScale );
		const float fTangentDirection = settings.m_fCliffSide < 0.0f ? -1.0f : 1.0f;
		const Vector3 tangent_axis =
			Vector3( b.x - a.x, fBottomB - fBottomA, b.z - a.z ).normalized() * fTangentDirection;
		const Vector3 raisedAxis( raised_normal.x, 0.0f, raised_normal.y );
		Vector3 vertical_axis = raisedAxis.cross( tangent_axis );
		vertical_axis = vertical_axis.length_squared() > s_fLandformEpsilon ? vertical_axis.normalized() : V3_UP;
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

Ref<ArrayMesh> T3DLandformBaker::buildCliffCollisionMesh(
	T3DTerrain *const pTerrain,
	const PackedVector3Array &arrPath,
	const Dictionary &dictSettings
) const {
	if ( !pTerrain || !pTerrain->getData() || arrPath.size() < 2 ) {
		return Ref<ArrayMesh>();
	}
	const t3dSettings_t settings = t3dLandformBakerParseSettings( dictSettings );
	const PackedVector3Array samples = t3dLandformBakerResamplePath( t3dLandformBakerSmoothPath( arrPath, 2 ), settings.m_fCollisionSpacing );
	if ( samples.size() < 2 ) {
		return Ref<ArrayMesh>();
	}

	typedef struct t3dCollisionSection_s {
		Vector3 m_top;
		Vector3 m_bottom;
	} t3dCollisionSection_t;
	eastl::vector<t3dCollisionSection_t> sections;
	sections.reserve( ( size_t ) ( samples.size() ) );
	T3DSampler sampler( pTerrain->getData()->getSamplingSourceInternal() );
	const float fVertexSpacing = MAX( pTerrain->getVertexSpacing(), 0.01f );

	for ( int index = 0; index < samples.size(); ++index ) {
		const Vector3 center = samples[index];
		const Vector3 previous = samples[MAX( index - 1, 0 )];
		const Vector3 next = samples[MIN( index + 1, samples.size() - 1 )];
		Vector2 tangent( next.x - previous.x, next.z - previous.z );
		tangent = tangent.length_squared() > s_fLandformEpsilon ? tangent.normalized() : Vector2( 1.0f, 0.0f );
		const Vector2 raised_normal = Vector2( -tangent.y, tangent.x ) * settings.m_fCliffSide;
		const Vector2 face_center = Vector2( center.x, center.z ) +
			raised_normal * ( settings.m_fAsymmetry * settings.m_fWidth * 0.125f );
		const float fEdgeSample = MAX( fVertexSpacing * 1.5f, settings.m_fWidth * 0.10f );
		const float fHighHeight = t3dLandformBakerSampleHeightOr( sampler, face_center + raised_normal * fEdgeSample,
			center.y + settings.m_fAmplitude );
		const float fLowHeight = t3dLandformBakerSampleHeightOr( sampler, face_center - raised_normal * fEdgeSample, center.y );
		const float fBottomHeight = MIN( fLowHeight - settings.m_fMeshToe, fHighHeight - 0.25f );
		sections.push_back( {Vector3( face_center.x, fHighHeight, face_center.y ),
			Vector3( face_center.x, fBottomHeight, face_center.y )} );
	}

	Ref<SurfaceTool> pSurface;
	pSurface.instantiate();
	pSurface->begin( Mesh::PRIMITIVE_TRIANGLES );
	for ( size_t index = 0; index + 1 < sections.size(); ++index ) {
		const t3dCollisionSection_t &a = sections[index];
		const t3dCollisionSection_t &b = sections[index + 1];
		t3dLandformBakerAddQuad( pSurface, a.m_top, a.m_bottom, b.m_bottom, b.m_top, V2_ZERO, V2_ZERO, V2_ZERO, V2_ZERO );
	}
	return pSurface->commit();
}

Ref<ArrayMesh> T3DLandformBaker::buildCliffMesh(
	T3DTerrain *const pTerrain, const PackedVector3Array &arrPath,
	const Dictionary &dictSettings
) const {
	if ( !pTerrain || !pTerrain->getData() || arrPath.size() < 2 ) {
		return Ref<ArrayMesh>();
	}
	const t3dSettings_t settings = t3dLandformBakerParseSettings( dictSettings );
	const PackedVector3Array samples = t3dLandformBakerResamplePath( t3dLandformBakerSmoothPath( arrPath, 2 ), settings.m_fMeshSampleSpacing );
	if ( samples.size() < 2 ) {
		return Ref<ArrayMesh>();
	}

	float pFUvScale = 0.1f;
	if ( pTerrain->getAssets().is_valid() ) {
		const Ref<T3DTextureAsset> pAsset = pTerrain->getAssets()->getTextureAsset( settings.m_nBaseTexture );
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

	typedef struct t3dCrossSection_s {
		Vector3 m_highGround;
		Vector3 m_frontTop;
		Vector3 m_frontUpper;
		Vector3 m_frontLower;
		Vector3 m_frontToe;
		Vector3 m_lowGround;
		float m_fU = 0.0f;
		float m_fFaceHeight = 1.0f;
	} t3dCrossSection_t;
	eastl::vector<t3dCrossSection_t> sections;
	sections.reserve( ( size_t ) ( samples.size() ) );

	for ( int index = 0; index < samples.size(); ++index ) {
		const Vector3 center = samples[index];
		const Vector3 previous = samples[MAX( index - 1, 0 )];
		const Vector3 next = samples[MIN( index + 1, samples.size() - 1 )];
		Vector2 tangent( next.x - previous.x, next.z - previous.z );
		if ( tangent.length_squared() <= s_fLandformEpsilon ) {
			tangent = Vector2( 1.0f, 0.0f );
		} else {
			tangent = tangent.normalized();
		}

		const Vector2 positiveNormal( -tangent.y, tangent.x );
		const Vector2 raised_normal = positiveNormal * settings.m_fCliffSide;
		const Vector2 centerXz( center.x, center.z );
		const float fTransitionOffset = settings.m_fAsymmetry * settings.m_fWidth * 0.125f;
		const Vector2 face_center = centerXz + raised_normal * fTransitionOffset;
		const float fEdgeSample = MAX( fVertexSpacing * 1.5f, settings.m_fWidth * 0.10f );
		const float fMaximumUpperDepth = MAX( fEdgeSample, settings.m_fWidth * 0.42f );
		const float fMaximumLowerDepth = MAX( fEdgeSample, settings.m_fWidth * 0.32f );
		const float fUpperDepth = CLAMP( settings.m_fMeshDepth + settings.m_fMeshLip, fEdgeSample, fMaximumUpperDepth );
		const float fLowerDepth = CLAMP( settings.m_fMeshDepth * 0.55f, fEdgeSample, fMaximumLowerDepth );
		const Vector2 high_near_xz = face_center + raised_normal * fEdgeSample;
		const Vector2 low_near_xz = face_center - raised_normal * fEdgeSample;
		const Vector2 high_ground_xz = face_center + raised_normal * fUpperDepth;
		const Vector2 low_ground_xz = face_center - raised_normal * fLowerDepth;


		const float fHighNear = t3dLandformBakerSampleHeightOr( sampler, high_near_xz, center.y + settings.m_fAmplitude );
		const float fHighGroundHeight = t3dLandformBakerSampleHeightOr( sampler, high_ground_xz, fHighNear );
		const float fLowNear = t3dLandformBakerSampleHeightOr( sampler, low_near_xz, center.y );
		const float fLowGroundHeight = t3dLandformBakerSampleHeightOr( sampler, low_ground_xz, fLowNear );
		const float fTopHeight = fHighNear;
		const float fFaceBottom = MIN( fLowNear - settings.m_fMeshToe, fTopHeight - 0.25f );
		const float face_height = fTopHeight - fFaceBottom;

		const float fNoiseSpacing = MAX( settings.m_fMeshSampleSpacing, 0.25f );
		const int noise_x = ( int ) ( Math::floor( center.x / fNoiseSpacing ) );
		const int noise_z = ( int ) ( Math::floor( center.z / fNoiseSpacing ) );
		const float fTopNoise = t3dLandformBakerSignedNoise( noise_x, noise_z, settings.m_nSeed ^ 0x517cc1b7 );
		const float fUpperNoise = t3dLandformBakerSignedNoise( noise_x, noise_z, settings.m_nSeed ^ 0x68bc21ebu );
		const float fLowerNoise = t3dLandformBakerSignedNoise( noise_x, noise_z, settings.m_nSeed ^ 0x02e5be93u );
		const float fToeNoise = t3dLandformBakerSignedNoise( noise_x, noise_z, settings.m_nSeed ^ 0x967a889bu );
		const float fMaxOffset = settings.m_fMeshDepth * 0.35f * settings.m_fMeshIrregularity;
		const Vector2 top_xz = face_center + raised_normal * ( fTopNoise * fMaxOffset * 0.25f );
		const Vector2 upper_face_xz = face_center + raised_normal * ( fUpperNoise * fMaxOffset );
		const Vector2 lower_face_xz = face_center + raised_normal * ( fLowerNoise * fMaxOffset );
		const Vector2 toe_xz = face_center + raised_normal * ( fToeNoise * fMaxOffset * 0.45f );
		const float fUpperBand = fFaceBottom + face_height * 0.70f + fUpperNoise * face_height * 0.035f;
		const float fLowerBand = fFaceBottom + face_height * 0.34f + fLowerNoise * face_height * 0.045f;
		if ( index > 0 ) {
			fCumulativeDistance += t3dLandformBakerSegmentLengthXz( samples[index - 1], center );
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
		const t3dCrossSection_t &a = sections[index];
		const t3dCrossSection_t &b = sections[index + 1];
		const float fAVUpper = ( a.m_frontTop.y - a.m_frontUpper.y ) * pFUvScale;
		const float fAVLower = ( a.m_frontTop.y - a.m_frontLower.y ) * pFUvScale;
		const float fAVToe = a.m_fFaceHeight * pFUvScale;
		const float fAVGround = fAVToe + a.m_frontToe.distance_to( a.m_lowGround ) * pFUvScale;
		const float fBVUpper = ( b.m_frontTop.y - b.m_frontUpper.y ) * pFUvScale;
		const float fBVLower = ( b.m_frontTop.y - b.m_frontLower.y ) * pFUvScale;
		const float fBVToe = b.m_fFaceHeight * pFUvScale;
		const float fBVGround = fBVToe + b.m_frontToe.distance_to( b.m_lowGround ) * pFUvScale;

		t3dLandformBakerAddQuad( pSurface, a.m_frontTop, a.m_frontUpper, b.m_frontUpper, b.m_frontTop, Vector2( a.m_fU, 0.0f ),
				 Vector2( a.m_fU, fAVUpper ), Vector2( b.m_fU, fBVUpper ), Vector2( b.m_fU, 0.0f ) );
		t3dLandformBakerAddQuad( pSurface, a.m_frontUpper, a.m_frontLower, b.m_frontLower, b.m_frontUpper, Vector2( a.m_fU, fAVUpper ),
				 Vector2( a.m_fU, fAVLower ), Vector2( b.m_fU, fBVLower ), Vector2( b.m_fU, fBVUpper ) );
		t3dLandformBakerAddQuad( pSurface, a.m_frontLower, a.m_frontToe, b.m_frontToe, b.m_frontLower, Vector2( a.m_fU, fAVLower ),
				 Vector2( a.m_fU, fAVToe ), Vector2( b.m_fU, fBVToe ), Vector2( b.m_fU, fBVLower ) );
		t3dLandformBakerAddQuad( pSurface, a.m_highGround, a.m_frontTop, b.m_frontTop, b.m_highGround,
				 Vector2( a.m_fU, settings.m_fMeshDepth * pFUvScale ), Vector2( a.m_fU, 0.0f ), Vector2( b.m_fU, 0.0f ),
				 Vector2( b.m_fU, settings.m_fMeshDepth * pFUvScale ) );
		t3dLandformBakerAddQuad( pSurface, a.m_frontToe, a.m_lowGround, b.m_lowGround, b.m_frontToe, Vector2( a.m_fU, fAVToe ),
				 Vector2( a.m_fU, fAVGround ), Vector2( b.m_fU, fBVGround ), Vector2( b.m_fU, fBVToe ) );
	}

	const t3dCrossSection_t &first = sections.front();
	const t3dCrossSection_t &last = sections.back();
	t3dLandformBakerAddTriangle( pSurface, first.m_highGround, first.m_frontUpper, first.m_frontTop, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.5f ), Vector2( 1.0f, 0.0f ) );
	t3dLandformBakerAddTriangle( pSurface, first.m_highGround, first.m_frontLower, first.m_frontUpper, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.8f ), Vector2( 0.5f, 0.5f ) );
	t3dLandformBakerAddTriangle( pSurface, first.m_highGround, first.m_frontToe, first.m_frontLower, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 0.9f ), Vector2( 0.5f, 0.8f ) );
	t3dLandformBakerAddTriangle( pSurface, first.m_highGround, first.m_lowGround, first.m_frontToe, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 1.0f ), Vector2( 1.0f, 0.9f ) );
	t3dLandformBakerAddTriangle( pSurface, last.m_highGround, last.m_frontTop, last.m_frontUpper, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 0.0f ), Vector2( 0.5f, 0.5f ) );
	t3dLandformBakerAddTriangle( pSurface, last.m_highGround, last.m_frontUpper, last.m_frontLower, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.5f ), Vector2( 0.5f, 0.8f ) );
	t3dLandformBakerAddTriangle( pSurface, last.m_highGround, last.m_frontLower, last.m_frontToe, Vector2( 0.0f, 0.0f ),
				 Vector2( 0.5f, 0.8f ), Vector2( 1.0f, 0.9f ) );
	t3dLandformBakerAddTriangle( pSurface, last.m_highGround, last.m_frontToe, last.m_lowGround, Vector2( 0.0f, 0.0f ),
				 Vector2( 1.0f, 0.9f ), Vector2( 1.0f, 1.0f ) );

	pSurface->generate_normals();
	pSurface->generate_tangents();
	Ref<ArrayMesh> pMesh = pSurface->commit();
	if ( pMesh.is_valid() && pMesh->get_surface_count() > 0 ) {
		pMesh->surface_set_material( 0, t3dLandformBakerCreateCliffMaterial( pTerrain, settings, m_materialCache ) );
	}
	return pMesh;
}

void T3DLandformBaker::_bind_methods( void ) {
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

	ClassDB::bind_method( D_METHOD( "apply", "terrain", "path", "settings" ), &T3DLandformBaker::apply );
	ClassDB::bind_method( D_METHOD( "restore", "terrain", "snapshots", "edited_area" ), &T3DLandformBaker::restore );
	ClassDB::bind_method( D_METHOD( "build_cliff_mesh", "terrain", "path", "settings" ),
						 &T3DLandformBaker::buildCliffMesh );
	ClassDB::bind_method( D_METHOD( "build_cliff_modules", "terrain", "path", "settings" ),
						 &T3DLandformBaker::buildCliffModules );
	ClassDB::bind_method( D_METHOD( "build_cliff_collision_mesh", "terrain", "path", "settings" ),
						 &T3DLandformBaker::buildCliffCollisionMesh );
}
