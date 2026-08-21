// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/terrain_3d_autoshader_baker.h"

#include "authoring/terrain_3d_autoshader_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include "eastl_config.h"
#include <EASTL/vector.h>

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "terrain_3d.h"
#include "assets/terrain_3d_assets.h"
#include "data/terrain_3d_data.h"
#include "memory/terrain_3d_linear_arena.h"
#include "simd/terrain_3d_parallel.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_sampler.h"
#include "core/terrain_3d_util.h"

namespace {
EA_FORCE_INLINE static bool terrain3dTextureSlotIsPopulated( const Ref<T3DAssets> &rAssets, const uint8_t nTextureId ) { return rAssets.is_valid() && rAssets->getTextureAsset( ( int ) ( nTextureId ) ).is_valid(); }

typedef struct t3dMaskSource_s {
	Ref<Image> pImage;
	PackedByteArray rgba_float_data;
	Rect2 world_rect;
	int nWidth = 0;
	int nHeight = 0;
	int channel = 0;
	bool bInvert = false;

	EA_FORCE_INLINE bool valid( void ) const { return nWidth > 0 && nHeight > 0 && !rgba_float_data.is_empty() && world_rect.size.x != 0.0f && world_rect.size.y != 0.0f; }
} t3dMaskSource_t;

typedef struct t3dRuleConfig_s {
	bool bEnabled = true;
	int priority = 0;
	int order = 0;
	uint8_t base_texture = 0u;
	uint8_t overlay_texture = 0u;
	float fBlend = 0.0f;
	float fSlopeFalloff = 0.0f;
	float fHeightFalloff = 0.0f;
	float fCurvatureFalloff = 0.0f;
	float fConcavityFalloff = 0.0f;
	float fAspectFalloff = 0.0f;
	float fFlowFalloff = 0.0f;
	float fDepositionFalloff = 0.0f;
	float fWaterDistanceFalloff = 0.0f;
	float fNoiseFalloff = 0.0f;
	float fBiomeFalloff = 0.0f;
	bool bOverwriteManual = false;
	int curve_mode = 1;
	eastl::vector<float> curve_lut;

	bool bUseSlope = false;
	Vector2 slope_range = Vector2( 0.0f, 90.0f );
	bool bUseHeight = false;
	Vector2 heightRange = Vector2( -384.0f, 768.0f );
	bool bUseCurvature = false;
	Vector2 curvature_range = Vector2( -1.0f, 1.0f );
	bool bUseConcavity = false;
	Vector2 concavity_range = Vector2( 0.0f, 1.0f );
	bool bUseAspect = false;
	float fAspectCenter = 0.0f;
	float fAspectWidth = 360.0f;
	bool bUseFlow = false;
	Vector2 flow_range = Vector2( 0.0f, 1.0f );
	bool bUseDeposition = false;
	Vector2 deposition_range = Vector2( 0.0f, 1.0f );
	bool bUseWaterDistance = false;
	Vector2 water_distance_range = Vector2( 0.0f, 100000.0f );
	bool bUseNoise = false;
	Vector2 noise_range = Vector2( 0.0f, 1.0f );
	bool bUseBiome = false;
	Vector2 biome_range = Vector2( 0.0f, 1.0f );
	bool bUseExistingMaterial = false;
	int existing_base = -1;
	int existing_overlay = -1;
} t3dRuleConfig_t;

typedef struct t3dRegionWork_s {
	Vector2i location;
	bool available = false;
	bool success = false;
	bool changed = false;
	int64_t changed_pixels = 0;
	int64_t skipped_manual = 0;
	PackedByteArray controlBytes;
	PackedByteArray outputBytes;
} t3dRegionWork_t;

typedef struct t3dClearRegionWork_s {
	Vector2i location;
	PackedByteArray output_bytes;
	int64_t changed_pixels = 0;
	bool changed = false;
} t3dClearRegionWork_t;

EA_FORCE_INLINE static bool terrain3dRulePriorityGreater( const t3dRuleConfig_t &rA, const t3dRuleConfig_t &rB ) { return rA.priority > rB.priority; }

typedef struct t3dMetricNeeds_s {
	bool bSlope = false;
	bool bCurvature = false;
	bool bConcavity = false;
	bool bAspect = false;
	bool bFlow = false;
	bool bDeposition = false;
	bool bWaterDistance = false;
	bool noise = false;
	bool bBiome = false;
} t3dMetricNeeds_t;

typedef struct t3dMetricBuildConfig_s {
	float fCurvatureScale = 1.0f;
	float water_level = 0.0f;
	float noise_scale = 0.01f;
	int noise_octaves = 4;
	float fNoiseLacunarity = 2.0f;
	float fNoiseGain = 0.5f;
	uint32_t noise_seed = 1337u;
} t3dMetricBuildConfig_t;

static t3dMetricBuildConfig_t parseMetricBuildConfig( const Dictionary &rProfile ) {
	t3dMetricBuildConfig_t config;
	config.fCurvatureScale = MAX( ( float ) ( rProfile.get( "curvature_scale", 1.0f ) ), 0.0001f );
	config.water_level = ( float ) ( rProfile.get( "water_level", 0.0f ) );
	config.noise_scale = MAX( ( float ) ( rProfile.get( "noise_scale", 0.01f ) ), 0.000001f );
	config.noise_octaves = CLAMP( ( int ) ( rProfile.get( "noise_octaves", 4 ) ), 1, 8 );
	config.fNoiseLacunarity = MAX( ( float ) ( rProfile.get( "noise_lacunarity", 2.0f ) ), 1.0f );
	config.fNoiseGain = CLAMP( ( float ) ( rProfile.get( "noise_gain", 0.5f ) ), 0.0f, 1.0f );
	config.noise_seed = ( uint32_t ) ( ( int64_t ) ( rProfile.get( "noise_seed", 1337 ) ) );
	return config;
}

typedef struct t3dRegionMetrics_s {
	T3DArenaBuffer<float> height;
	T3DArenaBuffer<float> height_halo;
	T3DArenaBuffer<float> slope;
	T3DArenaBuffer<float> curvature;
	T3DArenaBuffer<float> concavity;
	T3DArenaBuffer<float> aspect;
	T3DArenaBuffer<float> flow;
	T3DArenaBuffer<float> deposition;
	T3DArenaBuffer<float> water_distance;
	T3DArenaBuffer<float> noise;
	T3DArenaBuffer<float> biome;
	T3DArenaBuffer<int32_t> flow_order;
	T3DArenaBuffer<uint32_t> output_control;
} t3dRegionMetrics_t;


typedef struct t3dFlowOrderHeightGreater_s {
	const float *height = nullptr;

	bool operator()( const int32_t nA, const int32_t nB ) const {
		return height[( size_t ) ( nA )] > height[( size_t ) ( nB )];
	}
} t3dFlowOrderHeightGreater_t;

EA_FORCE_INLINE static Vector2 dictionaryRange( const Dictionary &rDictionary, const StringName &rName, const Vector2 &vDefault ) { const Variant value = rDictionary.get( rName, vDefault ); return value.get_type() == Variant::VECTOR2 ? Vector2( value ) : vDefault; }

static t3dMaskSource_t parseMask( const Dictionary &rProfile, const StringName &rPrefix ) {
	t3dMaskSource_t result;
	const String prefix = String( rPrefix );
	const Variant image_value = rProfile.get( prefix + String( "_image" ), Variant() );
	if ( image_value.get_type() == Variant::OBJECT ) {
		result.pImage = image_value;
	}
	const Variant rect_value = rProfile.get( prefix + String( "_world_rect" ), Rect2() );
	if ( rect_value.get_type() == Variant::RECT2 ) {
		result.world_rect = rect_value;
	}
	result.channel = CLAMP( ( int ) ( rProfile.get( prefix + String( "_channel" ), 0 ) ), 0, 3 );
	result.bInvert = ( bool ) ( rProfile.get( prefix + String( "_invert" ), false ) );
	if ( result.pImage.is_valid() && !result.pImage->is_empty() ) {
		Ref<Image> pNormalized = Ref<Image>( result.pImage->duplicate() );
		if ( pNormalized.is_valid() ) {
			pNormalized->convert( Image::FORMAT_RGBAF );
			result.nWidth = pNormalized->get_width();
			result.nHeight = pNormalized->get_height();
			result.rgba_float_data = pNormalized->get_data();
			const size_t expected = ( size_t ) ( result.nWidth ) * ( size_t ) ( result.nHeight ) * 4u * sizeof( float );
			if ( ( size_t ) ( result.rgba_float_data.size() ) < expected ) {
				result.rgba_float_data.clear();
				result.nWidth = 0;
				result.nHeight = 0;
			}
		}
	}
	return result;
}


static float sampleMask(
	const t3dMaskSource_t &rMask, const float fWorldX, const float fWorldZ,
	const float fDefault
) {
	if ( !rMask.valid() ) {
		return fDefault;
	}
	const float fU = ( fWorldX - rMask.world_rect.position.x ) / rMask.world_rect.size.x;
	const float fV = ( fWorldZ - rMask.world_rect.position.y ) / rMask.world_rect.size.y;
	if ( fU < 0.0f || fU > 1.0f || fV < 0.0f || fV > 1.0f ) {
		return fDefault;
	}
	const int nWidth = rMask.nWidth;
	const int nHeight = rMask.nHeight;
	const int x = CLAMP( ( int ) ( Math::round( fU * ( float ) ( nWidth - 1 ) ) ), 0, nWidth - 1 );
	const int y = CLAMP( ( int ) ( Math::round( fV * ( float ) ( nHeight - 1 ) ) ), 0, nHeight - 1 );
	float fValue = 0.0f;
	const size_t component = ( ( size_t ) ( y ) * ( size_t ) ( nWidth ) + ( size_t ) ( x ) ) * 4u + ( size_t ) ( rMask.channel );
	std::memcpy( &fValue, rMask.rgba_float_data.ptr() + component * sizeof( float ), sizeof( float ) );
	if ( rMask.bInvert ) {
		fValue = 1.0f - fValue;
	}
	return CLAMP( fValue, 0.0f, 1.0f );
}

using terrain3d_autoshader_math::angleDistanceDegrees;
using terrain3d_autoshader_math::fbmNoise;
using terrain3d_autoshader_math::rangeWeight;
using terrain3d_autoshader_math::ruleCanProcessPixel;
using terrain3d_autoshader_math::smoothCurve;
using terrain3d_autoshader_math::smootherCurve;

static float applyRuleCurve( const t3dRuleConfig_t &rRule, const float fValue ) {
	const float value = CLAMP( fValue, 0.0f, 1.0f );
	if ( rRule.curve_lut.size() >= 2u ) {
		const float fPosition = value * ( float ) ( rRule.curve_lut.size() - 1u );
		const size_t lower = ( size_t ) ( CLAMP( ( int ) ( Math::floor( fPosition ) ), 0, ( int ) ( rRule.curve_lut.size() - 1u ) ) );
		const size_t upper = MIN( lower + 1u, rRule.curve_lut.size() - 1u );
		return CLAMP( Math::lerp( rRule.curve_lut[lower], rRule.curve_lut[upper], fPosition - ( float ) ( lower ) ), 0.0f,
					 1.0f );
	}
	switch ( rRule.curve_mode ) {
		case 0:
			return value;
		case 2:
			return smootherCurve( value );
		default:
			return smoothCurve( value );
	}
}

static t3dRuleConfig_t parseRule( const Dictionary &rDictionary, const int nOrder ) {
	t3dRuleConfig_t rule;
	rule.bEnabled = ( bool ) ( rDictionary.get( "enabled", true ) );
	rule.priority = ( int ) ( rDictionary.get( "priority", 0 ) );
	rule.order = nOrder;
	rule.base_texture = ( uint8_t ) ( CLAMP( ( int ) ( rDictionary.get( "base_texture", 0 ) ), 0, 31 ) );
	rule.overlay_texture = ( uint8_t ) ( CLAMP( ( int ) ( rDictionary.get( "overlay_texture", 0 ) ), 0, 31 ) );
	rule.fBlend = CLAMP( ( float ) ( rDictionary.get( "blend", 0.0f ) ), 0.0f, 1.0f );
	const float fLegacyFalloff = MAX( ( float ) ( rDictionary.get( "falloff", 0.0f ) ), 0.0f );
	rule.fSlopeFalloff = MAX( ( float ) ( rDictionary.get( "slope_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fHeightFalloff = MAX( ( float ) ( rDictionary.get( "height_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fCurvatureFalloff = MAX( ( float ) ( rDictionary.get( "curvature_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fConcavityFalloff = MAX( ( float ) ( rDictionary.get( "concavity_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fAspectFalloff = MAX( ( float ) ( rDictionary.get( "aspect_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fFlowFalloff = MAX( ( float ) ( rDictionary.get( "flow_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fDepositionFalloff = MAX( ( float ) ( rDictionary.get( "deposition_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fWaterDistanceFalloff = MAX( ( float ) ( rDictionary.get( "water_distance_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fNoiseFalloff = MAX( ( float ) ( rDictionary.get( "noise_falloff", fLegacyFalloff ) ), 0.0f );
	rule.fBiomeFalloff = MAX( ( float ) ( rDictionary.get( "biome_falloff", fLegacyFalloff ) ), 0.0f );
	rule.bOverwriteManual = ( bool ) ( rDictionary.get( "overwrite_manual", false ) );
	rule.curve_mode = CLAMP( ( int ) ( rDictionary.get( "curve_mode", 1 ) ), 0, 2 );
	const Variant curve_value = rDictionary.get( "curve_lut", PackedFloat32Array() );
	if ( curve_value.get_type() == Variant::PACKED_FLOAT32_ARRAY ) {
		const PackedFloat32Array source = curve_value;
		rule.curve_lut.resize( ( size_t ) ( source.size() ) );
		for ( int index = 0; index < source.size(); ++index ) {
			rule.curve_lut[( size_t ) ( index )] = source[index];
		}
	}

	rule.bUseSlope = ( bool ) ( rDictionary.get( "use_slope", false ) );
	rule.slope_range = dictionaryRange( rDictionary, "slope_range", rule.slope_range );
	rule.bUseHeight = ( bool ) ( rDictionary.get( "use_height", false ) );
	rule.heightRange = dictionaryRange( rDictionary, "height_range", rule.heightRange );
	rule.bUseCurvature = ( bool ) ( rDictionary.get( "use_curvature", false ) );
	rule.curvature_range = dictionaryRange( rDictionary, "curvature_range", rule.curvature_range );
	rule.bUseConcavity = ( bool ) ( rDictionary.get( "use_concavity", false ) );
	rule.concavity_range = dictionaryRange( rDictionary, "concavity_range", rule.concavity_range );
	rule.bUseAspect = ( bool ) ( rDictionary.get( "use_aspect", false ) );
	rule.fAspectCenter = ( float ) ( rDictionary.get( "aspect_center", 0.0f ) );
	rule.fAspectWidth = CLAMP( ( float ) ( rDictionary.get( "aspect_width", 360.0f ) ), 0.0f, 360.0f );
	rule.bUseFlow = ( bool ) ( rDictionary.get( "use_flow", false ) );
	rule.flow_range = dictionaryRange( rDictionary, "flow_range", rule.flow_range );
	rule.bUseDeposition = ( bool ) ( rDictionary.get( "use_deposition", false ) );
	rule.deposition_range = dictionaryRange( rDictionary, "deposition_range", rule.deposition_range );
	rule.bUseWaterDistance = ( bool ) ( rDictionary.get( "use_water_distance", false ) );
	rule.water_distance_range = dictionaryRange( rDictionary, "water_distance_range", rule.water_distance_range );
	rule.bUseNoise = ( bool ) ( rDictionary.get( "use_noise", false ) );
	rule.noise_range = dictionaryRange( rDictionary, "noise_range", rule.noise_range );
	rule.bUseBiome = ( bool ) ( rDictionary.get( "use_biome", false ) );
	rule.biome_range = dictionaryRange( rDictionary, "biome_range", rule.biome_range );
	rule.bUseExistingMaterial = ( bool ) ( rDictionary.get( "use_existing_material", false ) );
	rule.existing_base = CLAMP( ( int ) ( rDictionary.get( "existing_base", -1 ) ), -1, 31 );
	rule.existing_overlay = CLAMP( ( int ) ( rDictionary.get( "existing_overlay", -1 ) ), -1, 31 );
	return rule;
}

static t3dMetricNeeds_t getMetricNeeds( const eastl::vector<t3dRuleConfig_t> &rRules ) {
	t3dMetricNeeds_t needs;
	for ( const t3dRuleConfig_t &rule : rRules ) {
		if ( !rule.bEnabled ) {
			continue;
		}
		needs.bSlope |= rule.bUseSlope || rule.bUseDeposition;
		needs.bCurvature |= rule.bUseCurvature || rule.bUseConcavity || rule.bUseDeposition;
		needs.bConcavity |= rule.bUseConcavity || rule.bUseDeposition;
		needs.bAspect |= rule.bUseAspect;
		needs.bFlow |= rule.bUseFlow || rule.bUseDeposition;
		needs.bDeposition |= rule.bUseDeposition;
		needs.bWaterDistance |= rule.bUseWaterDistance;
		needs.noise |= rule.bUseNoise;
		needs.bBiome |= rule.bUseBiome;
	}
	return needs;
}

EA_FORCE_INLINE static bool addMetricLayout( T3DLinearArenaLayout &rLayout, const bool bEnabled, const size_t nCount ) { return !bEnabled || rLayout.addArray<float>( nCount ); }

EA_FORCE_INLINE static bool allocateMetric( T3DArenaBuffer<float> &fBuffer, T3DLinearArena &rArena, const bool bEnabled, const size_t nCount ) { return !bEnabled || fBuffer.allocateUninitialized( rArena, nCount ); }

static void computeChamferDistance( float *pValues, const int nSize, const float fSpacing ) {
	for ( int y = 0; y < nSize; ++y ) {
		for ( int x = 0; x < nSize; ++x ) {
			const int index = y * nSize + x;
			float fValue = pValues[index];
			if ( x > 0 ) {
				fValue = MIN( fValue, pValues[index - 1] + fSpacing );
			}
			if ( y > 0 ) {
				fValue = MIN( fValue, pValues[index - nSize] + fSpacing );
				if ( x > 0 ) {
					fValue = MIN( fValue, pValues[index - nSize - 1] + fSpacing * TERRAIN3D_SQRT_TWO );
				}
				if ( x + 1 < nSize ) {
					fValue = MIN( fValue, pValues[index - nSize + 1] + fSpacing * TERRAIN3D_SQRT_TWO );
				}
			}
			pValues[index] = fValue;
		}
	}
	for ( int y = nSize - 1; y >= 0; --y ) {
		for ( int x = nSize - 1; x >= 0; --x ) {
			const int index = y * nSize + x;
			float fValue = pValues[index];
			if ( x + 1 < nSize ) {
				fValue = MIN( fValue, pValues[index + 1] + fSpacing );
			}
			if ( y + 1 < nSize ) {
				fValue = MIN( fValue, pValues[index + nSize] + fSpacing );
				if ( x > 0 ) {
					fValue = MIN( fValue, pValues[index + nSize - 1] + fSpacing * TERRAIN3D_SQRT_TWO );
				}
				if ( x + 1 < nSize ) {
					fValue = MIN( fValue, pValues[index + nSize + 1] + fSpacing * TERRAIN3D_SQRT_TWO );
				}
			}
			pValues[index] = fValue;
		}
	}
}

static bool buildMetrics(
	T3DData *pData, const Vector2i &rLocation, const t3dMetricNeeds_t &rNeeds,
	const t3dMetricBuildConfig_t &rConfig, const t3dMaskSource_t &rBiomeMask, const t3dMaskSource_t &rFlowMask,
	const t3dMaskSource_t &rDepositionMask, const t3dMaskSource_t &rWaterMask, T3DLinearArena &rArena,
	t3dRegionMetrics_t &rMetrics
) {
	const int size = pData->getRegionSizeInternal();
	const size_t count = ( size_t ) ( size ) * ( size_t ) ( size );
	const bool bNeedsNeighborhood = rNeeds.bSlope || rNeeds.bCurvature || rNeeds.bConcavity || rNeeds.bAspect;
	const int halo_pitch = size + 2;
	const size_t halo_count = ( size_t ) ( halo_pitch ) * ( size_t ) ( halo_pitch );
	T3DLinearArenaLayout layout;
	if ( !layout.addArray<float>( count ) ||
		( bNeedsNeighborhood && !layout.addArray<float>( halo_count ) ) ||
		!addMetricLayout( layout, rNeeds.bSlope, count ) ||
		!addMetricLayout( layout, rNeeds.bCurvature, count ) || !addMetricLayout( layout, rNeeds.bConcavity, count ) ||
		!addMetricLayout( layout, rNeeds.bAspect, count ) || !addMetricLayout( layout, rNeeds.bFlow, count ) ||
		!addMetricLayout( layout, rNeeds.bDeposition, count ) ||
		!addMetricLayout( layout, rNeeds.bWaterDistance, count ) || !addMetricLayout( layout, rNeeds.noise, count ) ||
		!addMetricLayout( layout, rNeeds.bBiome, count ) || ( rNeeds.bFlow && !layout.addArray<int32_t>( count ) ) ||
		!layout.addArray<uint32_t>( count ) || !layout.valid() || !rArena.ensureCapacity( layout.size() ) ) {
		return false;
	}
	if ( !rMetrics.height.allocateUninitialized( rArena, count ) ||
		( bNeedsNeighborhood && !rMetrics.height_halo.allocateUninitialized( rArena, halo_count ) ) ||
		!allocateMetric( rMetrics.slope, rArena, rNeeds.bSlope, count ) ||
		!allocateMetric( rMetrics.curvature, rArena, rNeeds.bCurvature, count ) ||
		!allocateMetric( rMetrics.concavity, rArena, rNeeds.bConcavity, count ) ||
		!allocateMetric( rMetrics.aspect, rArena, rNeeds.bAspect, count ) ||
		!allocateMetric( rMetrics.flow, rArena, rNeeds.bFlow, count ) ||
		!allocateMetric( rMetrics.deposition, rArena, rNeeds.bDeposition, count ) ||
		!allocateMetric( rMetrics.water_distance, rArena, rNeeds.bWaterDistance, count ) ||
		!allocateMetric( rMetrics.noise, rArena, rNeeds.noise, count ) ||
		!allocateMetric( rMetrics.biome, rArena, rNeeds.bBiome, count ) ||
		( rNeeds.bFlow && !rMetrics.flow_order.allocateUninitialized( rArena, count ) ) ||
		!rMetrics.output_control.allocateUninitialized( rArena, count ) ) {
		return false;
	}

	T3DSampler sampler( pData->getSamplingSourceInternal() );
	const int64_t origin_x = ( int64_t ) ( rLocation.x ) * ( int64_t ) ( size );
	const int64_t origin_z = ( int64_t ) ( rLocation.y ) * ( int64_t ) ( size );
	const float fSpacing = ( float ) ( pData->getVertexSpacingInternal() );
	const float curvature_scale = rConfig.fCurvatureScale;
	const float fWaterLevel = rConfig.water_level;
	const float fNoiseScale = rConfig.noise_scale;
	const int noise_octaves = rConfig.noise_octaves;
	const float noise_lacunarity = rConfig.fNoiseLacunarity;
	const float noise_gain = rConfig.fNoiseGain;
	const uint32_t noise_seed = rConfig.noise_seed;
	if ( bNeedsNeighborhood ) {
		sampler.copyHeightRect( origin_x - 1, origin_z - 1, halo_pitch, halo_pitch, rMetrics.height_halo.data(),
				halo_pitch, false );
	} else {
		sampler.copyHeightRect( origin_x, origin_z, size, size, rMetrics.height.data(), size, false );
	}

	for ( int y = 0; y < size; ++y ) {
		const float *pHaloRow = bNeedsNeighborhood
			? rMetrics.height_halo.data() + ( size_t ) ( y + 1 ) * ( size_t ) ( halo_pitch ) + 1u
			: nullptr;
		for ( int x = 0; x < size; ++x ) {
			const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
			const int64_t grid_x = origin_x + x;
			const int64_t grid_z = origin_z + y;
			float fCenter = bNeedsNeighborhood ? pHaloRow[x] : rMetrics.height[index];
			if ( !std::isfinite( fCenter ) ) {
				fCenter = 0.0f;
			}
			rMetrics.height[index] = fCenter;
			const float fWorldX = ( float ) ( grid_x ) * fSpacing;
			const float fWorldZ = ( float ) ( grid_z ) * fSpacing;
			if ( rNeeds.noise ) {
				rMetrics.noise[index] = fbmNoise( fWorldX * fNoiseScale, fWorldZ * fNoiseScale, noise_seed,
											  noise_octaves, noise_lacunarity, noise_gain );
			}
			if ( rNeeds.bBiome ) {
				rMetrics.biome[index] = sampleMask( rBiomeMask, fWorldX, fWorldZ, 0.0f );
			}
		}
	}

	if ( bNeedsNeighborhood ) {
		for ( int y = 0; y < size; ++y ) {
			const float *pHaloUp = rMetrics.height_halo.data() + ( size_t ) ( y ) * ( size_t ) ( halo_pitch );
			const float *pHaloCenter = pHaloUp + halo_pitch;
			const float *pHaloDown = pHaloCenter + halo_pitch;
			for ( int x = 0; x < size; ++x ) {
				const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
				const float fCenter = rMetrics.height[index];
				const int halo_x = x + 1;
				const float fLeftValue = pHaloCenter[halo_x - 1];
				const float fRightValue = pHaloCenter[halo_x + 1];
				const float fUpValue = pHaloUp[halo_x];
				const float fDownValue = pHaloDown[halo_x];
				const float fLeft = std::isfinite( fLeftValue ) ? fLeftValue : fCenter;
				const float fRight = std::isfinite( fRightValue ) ? fRightValue : fCenter;
				const float fUp = std::isfinite( fUpValue ) ? fUpValue : fCenter;
				const float fDown = std::isfinite( fDownValue ) ? fDownValue : fCenter;
				const float fDx = ( fRight - fLeft ) / ( 2.0f * fSpacing );
				const float fDz = ( fDown - fUp ) / ( 2.0f * fSpacing );
				if ( rNeeds.bSlope ) {
					rMetrics.slope[index] = Math::atan( Math::sqrt( fDx * fDx + fDz * fDz ) ) * TERRAIN3D_DEGREES_PER_RADIAN;
				}
				if ( rNeeds.bAspect ) {
					float fAspect = Math::atan2( -fDx, -fDz ) * TERRAIN3D_DEGREES_PER_RADIAN;
					if ( fAspect < 0.0f ) {
						fAspect += 360.0f;
					}
					rMetrics.aspect[index] = fAspect;
				}
				if ( rNeeds.bCurvature || rNeeds.bConcavity ) {
					const float fCurvature = CLAMP( ( ( fLeft + fRight + fUp + fDown ) * 0.25f - fCenter ) / curvature_scale,
											  -1.0f, 1.0f );
					if ( rNeeds.bCurvature ) {
						rMetrics.curvature[index] = fCurvature;
					}
					if ( rNeeds.bConcavity ) {
						rMetrics.concavity[index] = MAX( fCurvature, 0.0f );
					}
				}
			}
		}
	}

	if ( rNeeds.bFlow ) {
		if ( rFlowMask.valid() ) {
			for ( int y = 0; y < size; ++y ) {
				for ( int x = 0; x < size; ++x ) {
					const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
					const float fWorldX = ( float ) ( origin_x + x ) * fSpacing;
					const float fWorldZ = ( float ) ( origin_z + y ) * fSpacing;
					rMetrics.flow[index] = sampleMask( rFlowMask, fWorldX, fWorldZ, 0.0f );
				}
			}
		} else {
			for ( size_t index = 0; index < count; ++index ) {
				rMetrics.flow[index] = 1.0f;
				rMetrics.flow_order[index] = ( int32_t ) ( index );
			}
			EA_FORCE_INLINE std::sort( rMetrics.flow_order.begin(), rMetrics.flow_order.end(), t3dFlowOrderHeightGreater_t { rMetrics.height.data() } );
			float fMaximumFlow = 1.0f;
			for ( size_t order_index = 0; order_index < count; ++order_index ) {
				const int index = rMetrics.flow_order[order_index];
				const int x = index % size;
				const int y = index / size;
				const float fSourceHeight = rMetrics.height[( size_t ) ( index )];
				int destination = -1;
				float fDestinationHeight = fSourceHeight;
				for ( int offset_y = -1; offset_y <= 1; ++offset_y ) {
					for ( int offset_x = -1; offset_x <= 1; ++offset_x ) {
						if ( ( offset_x == 0 && offset_y == 0 ) || x + offset_x < 0 || x + offset_x >= size ||
							y + offset_y < 0 || y + offset_y >= size ) {
							continue;
						}
						const int neighbor = ( y + offset_y ) * size + x + offset_x;
						const float fNeighborHeight = rMetrics.height[( size_t ) ( neighbor )];
						if ( fNeighborHeight < fDestinationHeight ) {
							fDestinationHeight = fNeighborHeight;
							destination = neighbor;
						}
					}
				}
				if ( destination >= 0 ) {
					rMetrics.flow[( size_t ) ( destination )] += rMetrics.flow[( size_t ) ( index )];
					fMaximumFlow = MAX( fMaximumFlow, rMetrics.flow[( size_t ) ( destination )] );
				}
			}
			const float fDenominator = Math::log( 1.0f + fMaximumFlow );
			for ( size_t index = 0; index < count; ++index ) {
				rMetrics.flow[index] = fDenominator > 0.0f ? Math::log( 1.0f + rMetrics.flow[index] ) / fDenominator : 0.0f;
			}
		}
	}

	if ( rNeeds.bDeposition ) {
		for ( int y = 0; y < size; ++y ) {
			for ( int x = 0; x < size; ++x ) {
				const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
				const float fWorldX = ( float ) ( origin_x + x ) * fSpacing;
				const float fWorldZ = ( float ) ( origin_z + y ) * fSpacing;
				if ( rDepositionMask.valid() ) {
					rMetrics.deposition[index] = sampleMask( rDepositionMask, fWorldX, fWorldZ, 0.0f );
				} else {
					const float fFlatness = 1.0f - CLAMP( rMetrics.slope[index] / 90.0f, 0.0f, 1.0f );
					rMetrics.deposition[index] = CLAMP( rMetrics.concavity[index] * fFlatness * rMetrics.flow[index],
														0.0f, 1.0f );
				}
			}
		}
	}

	if ( rNeeds.bWaterDistance ) {
		for ( int y = 0; y < size; ++y ) {
			for ( int x = 0; x < size; ++x ) {
				const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
				const float fWorldX = ( float ) ( origin_x + x ) * fSpacing;
				const float fWorldZ = ( float ) ( origin_z + y ) * fSpacing;
				const bool bWater = rWaterMask.valid()
					? sampleMask( rWaterMask, fWorldX, fWorldZ, 0.0f ) >= 0.5f
					: rMetrics.height[index] <= fWaterLevel;
				rMetrics.water_distance[index] = bWater ? 0.0f : TERRAIN3D_LARGE_DISTANCE;
			}
		}
		computeChamferDistance( rMetrics.water_distance.data(), size, fSpacing );
	}
	return rArena.used() == rArena.capacity();
}

static float evaluateRule(
	const t3dRuleConfig_t &rRule, const t3dRegionMetrics_t &rMetrics, const size_t iIndex,
	const uint32_t nControl, const bool bAllowManual
) {
	if ( !rRule.bEnabled || ( !isAuto( nControl ) && !bAllowManual ) ) {
		return 0.0f;
	}
	if ( rRule.bUseExistingMaterial &&
		( ( rRule.existing_base >= 0 && ( int ) ( getBase( nControl ) ) != rRule.existing_base ) ||
		 ( rRule.existing_overlay >= 0 && ( int ) ( getOverlay( nControl ) ) != rRule.existing_overlay ) ) ) {
		return 0.0f;
	}
	float fScore = 1.0f;
	if ( rRule.bUseSlope ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.slope[iIndex], rRule.slope_range.x, rRule.slope_range.y,
								 rRule.fSlopeFalloff ) );
	}
	if ( rRule.bUseHeight ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.height[iIndex], rRule.heightRange.x, rRule.heightRange.y,
								 rRule.fHeightFalloff ) );
	}
	if ( rRule.bUseCurvature ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.curvature[iIndex], rRule.curvature_range.x,
								 rRule.curvature_range.y, rRule.fCurvatureFalloff ) );
	}
	if ( rRule.bUseConcavity ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.concavity[iIndex], rRule.concavity_range.x,
								 rRule.concavity_range.y, rRule.fConcavityFalloff ) );
	}
	if ( rRule.bUseAspect ) {
		const float fHalfWidth = rRule.fAspectWidth * 0.5f;
		const float fDistance = angleDistanceDegrees( rMetrics.aspect[iIndex], rRule.fAspectCenter );
		fScore = MIN( fScore, rangeWeight( fDistance, 0.0f, fHalfWidth, rRule.fAspectFalloff ) );
	}
	if ( rRule.bUseFlow ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.flow[iIndex], rRule.flow_range.x, rRule.flow_range.y,
								 rRule.fFlowFalloff ) );
	}
	if ( rRule.bUseDeposition ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.deposition[iIndex], rRule.deposition_range.x,
								 rRule.deposition_range.y, rRule.fDepositionFalloff ) );
	}
	if ( rRule.bUseWaterDistance ) {
		fScore = MIN( fScore,
					rangeWeight( rMetrics.water_distance[iIndex], rRule.water_distance_range.x,
								 rRule.water_distance_range.y, rRule.fWaterDistanceFalloff ) );
	}
	if ( rRule.bUseNoise ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.noise[iIndex], rRule.noise_range.x, rRule.noise_range.y,
								 rRule.fNoiseFalloff ) );
	}
	if ( rRule.bUseBiome ) {
		fScore = MIN( fScore, rangeWeight( rMetrics.biome[iIndex], rRule.biome_range.x, rRule.biome_range.y,
								 rRule.fBiomeFalloff ) );
	}
	return applyRuleCurve( rRule, fScore );
}

static uint64_t fnv1aBytes( const uint8_t *pBytes, const size_t nSize, uint64_t nHash = 1469598103934665603ull ) {
	for ( size_t index = 0; index < nSize; ++index ) {
		nHash ^= ( uint64_t ) ( pBytes[index] );
		nHash *= 1099511628211ull;
	}
	return nHash;
}

static uint64_t regionSignature( T3DRegion *pRegion, const uint64_t nProfileSignature ) {
	uint64_t result = nProfileSignature;
	if ( !pRegion ) {
		return result;
	}
	const Ref<Image> pHeight = pRegion->getHeightMap();
	if ( pHeight.is_valid() ) {
		const PackedByteArray bytes = pHeight->get_data();
		result = fnv1aBytes( bytes.ptr(), ( size_t ) ( bytes.size() ), result );
	}

	const int region_size = pRegion->getRegionSize();
	const size_t pixel_count = ( size_t ) ( region_size ) * ( size_t ) ( region_size );
	const Ref<Image> pControlImage = pRegion->getControlMap();
	PackedByteArray control_bytes;
	const uint8_t *pControlRead = nullptr;
	if ( pControlImage.is_valid() ) {
		control_bytes = pControlImage->get_data();
		if ( ( size_t ) ( control_bytes.size() ) >= pixel_count * sizeof( uint32_t ) ) {
			pControlRead = control_bytes.ptr();
		}
	}
	for ( size_t index = 0; index < pixel_count; ++index ) {
		uint32_t control = TERRAIN3D_REGION_DEFAULT_CONTROL;
		if ( pControlRead ) {
			std::memcpy( &control, pControlRead + index * sizeof( uint32_t ), sizeof( uint32_t ) );
		}
		// Baked material fields are outputs, so exclude them from auto-pixel
		// provenance. Manual pixels remain exact inputs for overwrite/filter rules.
		const uint32_t source_control = isAuto( control ) ? control & ~TERRAIN3D_AUTOSHADER_MATERIAL_MASK : control;
		result = fnv1aBytes( ( const uint8_t * ) ( &source_control ), sizeof( source_control ), result );
	}
	return result;
}

static TypedArray<Vector2i> normalizeLocations( T3DData *pData, const TypedArray<Vector2i> &rLocations ) {
	TypedArray<Vector2i> result;
	if ( !pData ) {
		return result;
	}
	if ( rLocations.is_empty() ) {
		return pData->getRegionLocations();
	}
	for ( int index = 0; index < rLocations.size(); ++index ) {
		const Vector2i location = rLocations[index];
		if ( pData->getRegionId( location ) >= 0 && !result.has( location ) ) {
			result.push_back( location );
		}
	}
	return result;
}

typedef struct t3dAutoShaderRegionContext_s {
	T3DData *data = nullptr;
	eastl::vector<t3dRegionWork_t> *pWork = nullptr;
	const eastl::vector<t3dRuleConfig_t> *pRules = nullptr;
	const t3dMetricNeeds_t *pNeeds = nullptr;
	const t3dMetricBuildConfig_t *pMetricConfig = nullptr;
	const t3dMaskSource_t *pBiomeMask = nullptr;
	const t3dMaskSource_t *pFlowMask = nullptr;
	const t3dMaskSource_t *pDepositionMask = nullptr;
	const t3dMaskSource_t *pWaterMask = nullptr;
	size_t pixel_count = 0u;
	bool bOnlyAutoPixels = true;
	bool bPreserveAutoMask = true;
	uint8_t default_base = 0u;
	uint8_t default_overlay = 0u;
	uint8_t default_blend = 0u;
	T3DLinearArena *pWorkerArenas = nullptr;
} t3dAutoShaderRegionContext_t;

static void terrain3dBakeAutoshaderRegions( void *pContext, const size_t nBegin, const size_t nEnd, const int32_t nWorker ) {
	t3dAutoShaderRegionContext_t *context = ( t3dAutoShaderRegionContext_t * ) ( pContext );
	for ( size_t work_index = nBegin; work_index < nEnd; ++work_index ) {
		t3dRegionWork_t &item = ( *context->pWork )[work_index];
		if ( !item.available ) {
			continue;
		}
		T3DLinearArena &arena = context->pWorkerArenas[MAX( nWorker, 0 )];
		t3dRegionMetrics_t metrics;
		if ( !buildMetrics( context->data, item.location, *context->pNeeds, *context->pMetricConfig,
				*context->pBiomeMask, *context->pFlowMask, *context->pDepositionMask, *context->pWaterMask,
				arena, metrics ) ) {
			continue;
		}
		item.success = true;
		const uint8_t *pControlRead = item.controlBytes.is_empty() ? nullptr : item.controlBytes.ptr();
		for ( size_t index = 0; index < context->pixel_count; ++index ) {
			uint32_t control = TERRAIN3D_REGION_DEFAULT_CONTROL;
			if ( pControlRead ) {
				std::memcpy( &control, pControlRead + index * sizeof( uint32_t ), sizeof( uint32_t ) );
			}
			metrics.output_control[index] = control;
			const bool bAutoPixel = isAuto( control );
			const t3dRuleConfig_t *pMatchedRule = nullptr;
			float fMatchedScore = 0.0f;
			for ( const t3dRuleConfig_t &rule : *context->pRules ) {
				const bool bAllowManual = ruleCanProcessPixel( bAutoPixel, context->bOnlyAutoPixels, rule.bOverwriteManual );
				const float fScore = evaluateRule( rule, metrics, index, control, bAllowManual );
				if ( fScore > 0.0f ) {
					pMatchedRule = &rule;
					fMatchedScore = fScore;
					break;
				}
			}
			if ( !bAutoPixel && context->bOnlyAutoPixels && !pMatchedRule ) {
				++item.skipped_manual;
				continue;
			}
			uint8_t base = context->default_base;
			uint8_t overlay = context->default_overlay;
			uint8_t blend = context->default_blend;
			if ( pMatchedRule ) {
				base = pMatchedRule->base_texture;
				overlay = pMatchedRule->overlay_texture;
				blend = ( uint8_t ) ( CLAMP( ( int ) ( Math::round( pMatchedRule->fBlend * fMatchedScore * TERRAIN3D_BYTE_MAX_FLOAT ) ), 0, 255 ) );
			}
			if ( base == overlay ) {
				blend = 0u;
			}
			uint32_t replacement = ( control & ~TERRAIN3D_AUTOSHADER_MATERIAL_MASK ) | encBase( base ) | encOverlay( overlay ) | encBlend( blend );
			if ( context->bPreserveAutoMask ) {
				replacement = ( replacement & ~0x1u ) | encAuto( isAuto( control ) );
			} else {
				replacement &= ~0x1u;
			}
			if ( replacement != control ) {
				metrics.output_control[index] = replacement;
				item.changed = true;
				++item.changed_pixels;
			}
		}
		if ( item.changed ) {
			item.outputBytes.resize( ( int64_t ) ( context->pixel_count * sizeof( uint32_t ) ) );
			std::memcpy( item.outputBytes.ptrw(), metrics.output_control.data(), context->pixel_count * sizeof( uint32_t ) );
		}
	}
}

} // namespace

Dictionary T3DAutoShaderBaker::bakeRegions(
	T3DTerrain *const pTerrain, const Dictionary &rProfile,
	const TypedArray<Vector2i> &rRegionLocations
) {
	Dictionary result;
	result["ok"] = false;
	result["processed_regions"] = 0;
	result["changed_regions"] = 0;
	result["changed_pixels"] = 0;
	result["skipped_manual_pixels"] = 0;
	result["errors"] = PackedStringArray();
	result["region_signatures"] = Dictionary();
	if ( !pTerrain || !pTerrain->getData() ) {
		result["error"] = "A valid Terrain3D node is required.";
		return result;
	}

	T3DData *const data = pTerrain->getData();
	const TypedArray<Vector2i> locations = normalizeLocations( data, rRegionLocations );
	if ( locations.is_empty() ) {
		result["error"] = "No valid terrain regions were supplied.";
		return result;
	}
	const Variant rules_value = rProfile.get( "rules", Array() );
	if ( rules_value.get_type() != Variant::ARRAY ) {
		result["error"] = "The autoshader profile has no rule array.";
		return result;
	}
	const Array rule_array = rules_value;
	eastl::vector<t3dRuleConfig_t> rules;
	rules.reserve( ( size_t ) ( rule_array.size() ) );
	for ( int index = 0; index < rule_array.size(); ++index ) {
		if ( rule_array[index].get_type() == Variant::DICTIONARY ) {
			rules.push_back( parseRule( rule_array[index], index ) );
		}
	}
	std::stable_sort( rules.begin(), rules.end(), terrain3dRulePriorityGreater );
	const t3dMetricNeeds_t needs = getMetricNeeds( rules );
	const bool only_auto_pixels = ( bool ) ( rProfile.get( "only_auto_pixels", true ) );
	const bool preserve_auto_mask = ( bool ) ( rProfile.get( "preserve_auto_mask", true ) );
	const uint8_t default_base = ( uint8_t ) ( CLAMP( ( int ) ( rProfile.get( "default_base_texture", 0 ) ), 0, 31 ) );
	const uint8_t default_overlay = ( uint8_t ) ( CLAMP( ( int ) ( rProfile.get( "default_overlay_texture", 0 ) ), 0, 31 ) );
	const uint8_t default_blend = ( uint8_t ) ( CLAMP( ( int ) ( Math::round( ( float ) ( rProfile.get( "default_blend", 0.0f ) ) * TERRAIN3D_BYTE_MAX_FLOAT ) ),
											 0, 255 ) );
	const Ref<T3DAssets> pAssets = pTerrain->getAssets();
	if ( pAssets.is_null() ) {
		result["error"] = "Terrain3D has no texture asset registry.";
		return result;
	}
	if ( !terrain3dTextureSlotIsPopulated( pAssets, default_base ) ||
			!terrain3dTextureSlotIsPopulated( pAssets, default_overlay ) ) {
		result["error"] = "The autoshader profile fallback references a missing terrain texture.";
		return result;
	}
	for ( const t3dRuleConfig_t &rule : rules ) {
		if ( !rule.bEnabled ) {
			continue;
		}
		if ( !terrain3dTextureSlotIsPopulated( pAssets, rule.base_texture ) ||
				!terrain3dTextureSlotIsPopulated( pAssets, rule.overlay_texture ) ) {
			result["error"] = String( "Material rule " ) + itos( rule.order + 1 ) + " references a missing terrain texture.";
			return result;
		}
	}
	const uint64_t profile_signature = ( uint64_t ) ( ( int64_t ) ( rProfile.get( "profile_signature", 0 ) ) );
	const t3dMaskSource_t biome_mask = parseMask( rProfile, "biome_mask" );
	const t3dMaskSource_t flow_mask = parseMask( rProfile, "flow_mask" );
	const t3dMaskSource_t deposition_mask = parseMask( rProfile, "deposition_mask" );
	const t3dMaskSource_t water_mask = parseMask( rProfile, "water_mask" );
	const t3dMetricBuildConfig_t metric_config = parseMetricBuildConfig( rProfile );

	eastl::vector<Vector2i> changed_locations;
	changed_locations.reserve( ( size_t ) ( locations.size() ) );
	Dictionary signatures;
	PackedStringArray errors;
	int64_t changed_pixel_count = 0;
	int64_t skipped_manual_count = 0;
	int processed_region_count = 0;

	const int size = data->getRegionSizeInternal();
	const size_t count = ( size_t ) ( size ) * ( size_t ) ( size );
	eastl::vector<t3dRegionWork_t> work( ( size_t ) ( locations.size() ) );
	for ( int location_index = 0; location_index < locations.size(); ++location_index ) {
		t3dRegionWork_t &item = work[( size_t ) ( location_index )];
		item.location = locations[location_index];
		if ( !data->ensureCpuRegionResident( item.location ) ) {
			continue;
		}
		T3DRegion *const pRegion = data->getRegionPtr( item.location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		item.available = true;
		const Ref<Image> pControlImage = pRegion->getControlMap();
		if ( pControlImage.is_valid() ) {
			item.controlBytes = pControlImage->get_data();
			if ( ( size_t ) item.controlBytes.size() < count * sizeof( uint32_t ) ) item.controlBytes.clear();
		}
	}

	const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	const size_t worker_count = MAX<size_t>( 1u, MIN<size_t>( ( size_t ) hardware_threads, work.size() ) );
	const T3DParallelExecutor executor( ( int32_t ) worker_count );
	std::unique_ptr<T3DLinearArena[]> worker_arenas( new T3DLinearArena[worker_count] );
	t3dAutoShaderRegionContext_t bake_context;
	bake_context.data = data;
	bake_context.pWork = &work;
	bake_context.pRules = &rules;
	bake_context.pNeeds = &needs;
	bake_context.pMetricConfig = &metric_config;
	bake_context.pBiomeMask = &biome_mask;
	bake_context.pFlowMask = &flow_mask;
	bake_context.pDepositionMask = &deposition_mask;
	bake_context.pWaterMask = &water_mask;
	bake_context.pixel_count = count;
	bake_context.bOnlyAutoPixels = only_auto_pixels;
	bake_context.bPreserveAutoMask = preserve_auto_mask;
	bake_context.default_base = default_base;
	bake_context.default_overlay = default_overlay;
	bake_context.default_blend = default_blend;
	bake_context.pWorkerArenas = worker_arenas.get();
	executor.runStatic( work.size(), 1u, terrain3dBakeAutoshaderRegions, &bake_context );

	// Validate the complete batch before mutating region resources. Editor callers
	// can therefore treat ok=false as "nothing was retained" without rebuilding a
	// second rollback path around partial native commits.
	for ( t3dRegionWork_t &item : work ) {
		if ( !data->ensureCpuRegionResident( item.location ) ) {
			errors.push_back( "Could not page CPU terrain maps for region " + String( item.location ) + "." );
			continue;
		}
		T3DRegion *const pRegion = data->getRegionPtr( item.location );
		if ( !item.available || !pRegion || pRegion->isDeleted() ) {
			errors.push_back( "Region " + String( item.location ) + " is unavailable." );
			continue;
		}
		if ( !item.success ) {
			errors.push_back( "Could not allocate or build metrics for region " + String( item.location ) + "." );
			continue;
		}
		++processed_region_count;
		changed_pixel_count += item.changed_pixels;
		skipped_manual_count += item.skipped_manual;
	}
	if ( !errors.is_empty() ) {
		result["processed_regions"] = processed_region_count;
		result["skipped_manual_pixels"] = skipped_manual_count;
		result["errors"] = errors;
		return result;
	}

	for ( t3dRegionWork_t &item : work ) {
		T3DRegion *const pRegion = data->getRegionPtr( item.location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( item.changed ) {
			const Ref<Image> pOutput = Image::create_from_data( size, size, false, Image::FORMAT_RF, item.outputBytes );
			pRegion->setControlMap( pOutput );
			changed_locations.push_back( item.location );
		}
		signatures[T3DUtil::locationToString( item.location )] =
			( int64_t ) ( regionSignature( pRegion, profile_signature ) );
	}

	if ( !changed_locations.empty() ) {
		data->commitMapEdits( TYPE_CONTROL, changed_locations, AABB(), MAP_EDIT_SNAP |
																		  MAP_EDIT_UPDATE_INSTANCES |
																		  MAP_EDIT_UPDATE_COLLISION );
	}
	result["ok"] = true;
	result["processed_regions"] = processed_region_count;
	result["changed_regions"] = ( int ) ( changed_locations.size() );
	result["changed_pixels"] = changed_pixel_count;
	result["skipped_manual_pixels"] = skipped_manual_count;
	result["errors"] = errors;
	result["region_signatures"] = signatures;
	return result;
}

Dictionary T3DAutoShaderBaker::computeRegionSignatures(
	T3DTerrain *const pTerrain, const Dictionary &rProfile, const TypedArray<Vector2i> &rRegionLocations
) {
	Dictionary signatures;
	if ( !pTerrain || !pTerrain->getData() ) {
		return signatures;
	}
	T3DData *const pData = pTerrain->getData();
	const TypedArray<Vector2i> locations = normalizeLocations( pData, rRegionLocations );
	const uint64_t profile_signature = ( uint64_t ) ( ( int64_t ) ( rProfile.get( "profile_signature", 0 ) ) );
	for ( int index = 0; index < locations.size(); ++index ) {
		const Vector2i location = locations[index];
		if ( !pData->ensureCpuRegionResident( location ) ) {
			continue;
		}
		T3DRegion *const pRegion = pData->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		signatures[T3DUtil::locationToString( location )] =
			( int64_t ) ( regionSignature( pRegion, profile_signature ) );
	}
	return signatures;
}

Dictionary T3DAutoShaderBaker::clearBakedRegions(
	T3DTerrain *const pTerrain,
	const TypedArray<Vector2i> &rRegionLocations,
	const bool bPreserveAutoMask
) {
	Dictionary result;
	result["ok"] = false;
	result["changed_regions"] = 0;
	result["changed_pixels"] = 0;
	result["errors"] = PackedStringArray();
	if ( !pTerrain || !pTerrain->getData() ) {
		result["error"] = "A valid Terrain3D node is required.";
		return result;
	}
	if ( !bPreserveAutoMask ) {
		result["error"] = "Selective clear requires the automatic-material mask to be preserved during baking.";
		return result;
	}
	T3DData *const pData = pTerrain->getData();
	const TypedArray<Vector2i> locations = normalizeLocations( pData, rRegionLocations );
	eastl::vector<t3dClearRegionWork_t> work;
	work.reserve( ( size_t ) ( locations.size() ) );
	PackedStringArray errors;
	for ( int location_index = 0; location_index < locations.size(); ++location_index ) {
		const Vector2i location = locations[location_index];
		if ( !pData->ensureCpuRegionResident( location ) ) {
			errors.push_back( "Could not page CPU terrain maps for region " + String( location ) + "." );
			continue;
		}
		T3DRegion *const pRegion = pData->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			errors.push_back( "Region " + String( location ) + " is unavailable." );
			continue;
		}
		if ( pRegion->getControlMap().is_null() ) {
			continue;
		}
		const int size = pRegion->getRegionSize();
		const size_t count = ( size_t ) ( size ) * ( size_t ) ( size );
		PackedByteArray bytes = pRegion->getControlMap()->get_data();
		if ( pRegion->getControlMap()->get_format() != Image::FORMAT_RF ||
			( size_t ) ( bytes.size() ) < count * sizeof( uint32_t ) ) {
			errors.push_back( "Region " + String( location ) + " has an invalid control map." );
			continue;
		}
		uint8_t *const pWrite = bytes.ptrw();
		t3dClearRegionWork_t item;
		item.location = location;
		for ( size_t index = 0; index < count; ++index ) {
			uint32_t control = 0u;
			std::memcpy( &control, pWrite + index * sizeof( uint32_t ), sizeof( uint32_t ) );
			if ( !isAuto( control ) ) {
				continue;
			}
			uint32_t replacement = control & ~TERRAIN3D_AUTOSHADER_MATERIAL_MASK;
			replacement = ( replacement & ~0x1u ) | encAuto( true );
			if ( replacement != control ) {
				std::memcpy( pWrite + index * sizeof( uint32_t ), &replacement, sizeof( uint32_t ) );
				item.changed = true;
				++item.changed_pixels;
			}
		}
		if ( item.changed ) {
			item.output_bytes = bytes;
			work.push_back( item );
		}
	}
	if ( !errors.is_empty() ) {
		result["errors"] = errors;
		return result;
	}

	eastl::vector<Vector2i> changed_locations;
	changed_locations.reserve( work.size() );
	int64_t changed_pixels = 0;
	for ( t3dClearRegionWork_t &item : work ) {
		T3DRegion *const pRegion = pData->getRegionPtr( item.location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const int size = pRegion->getRegionSize();
		pRegion->setControlMap( Image::create_from_data( size, size, false, Image::FORMAT_RF, item.output_bytes ) );
		changed_locations.push_back( item.location );
		changed_pixels += item.changed_pixels;
	}
	if ( !changed_locations.empty() ) {
		pData->commitMapEdits( TYPE_CONTROL, changed_locations, AABB(), MAP_EDIT_SNAP |
																		  MAP_EDIT_UPDATE_INSTANCES |
																		  MAP_EDIT_UPDATE_COLLISION );
	}
	result["ok"] = true;
	result["changed_regions"] = ( int ) ( changed_locations.size() );
	result["changed_pixels"] = changed_pixels;
	return result;
}

void T3DAutoShaderBaker::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "bake_regions", "terrain", "profile", "region_locations" ),
						 &T3DAutoShaderBaker::bakeRegions );
	ClassDB::bind_method( D_METHOD( "compute_region_signatures", "terrain", "profile", "region_locations" ),
						 &T3DAutoShaderBaker::computeRegionSignatures );
	ClassDB::bind_method( D_METHOD( "clear_baked_regions", "terrain", "region_locations", "preserve_auto_mask" ),
						 &T3DAutoShaderBaker::clearBakedRegions, DEFVAL( true ) );
}
