// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/terrain_3d_autoshader_baker.h"

#include "assets/terrain_3d_assets.h"
#include "authoring/terrain_3d_autoshader_math.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_data.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_sampler.h"
#include "eastl_config.h"
#include "memory/terrain_3d_linear_arena.h"
#include "simd/terrain_3d_parallel.h"
#include "terrain_3d.h"

#include <EASTL/sort.h>
#include <EASTL/vector.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <new>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

EA_FORCE_INLINE static bool t3dAutoshaderBakerTextureSlotIsPopulated( const Ref<Terrain3DAssets> &rAssets, const uint8_t nTextureId ) {
	return rAssets.is_valid() && rAssets->getTextureAsset( ( int )( nTextureId ) ).is_valid();
}

typedef struct t3dMaskSource_s {
	Ref<Image> m_image;
	PackedByteArray m_rgbaFloatData;
	Rect2 m_worldRect;
	int m_nWidth = 0;
	int m_nHeight = 0;
	int m_nChannel = 0;
	bool m_bInvert = false;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_nWidth > 0 && m_nHeight > 0 && !m_rgbaFloatData.is_empty() && m_worldRect.size.x != 0.0f && m_worldRect.size.y != 0.0f;
	}
} t3dMaskSource_t;

typedef struct t3dRuleConfig_s {
	bool m_bEnabled = true;
	int m_nPriority = 0;
	int m_nOrder = 0;
	uint8_t m_nBaseTexture = 0u;
	uint8_t m_nOverlayTexture = 0u;
	float m_fBlend = 0.0f;
	float m_fSlopeFalloff = 0.0f;
	float m_fHeightFalloff = 0.0f;
	float m_fCurvatureFalloff = 0.0f;
	float m_fConcavityFalloff = 0.0f;
	float m_fAspectFalloff = 0.0f;
	float m_fFlowFalloff = 0.0f;
	float m_fDepositionFalloff = 0.0f;
	float m_fWaterDistanceFalloff = 0.0f;
	float m_fNoiseFalloff = 0.0f;
	float m_fBiomeFalloff = 0.0f;
	bool m_bOverwriteManual = false;
	int m_nCurveMode = 1;
	eastl::vector<float> m_curveLut;

	bool m_bUseSlope = false;
	Vector2 m_slopeRange = Vector2( 0.0f, 90.0f );
	bool m_bUseHeight = false;
	Vector2 m_heightRange = Vector2( -384.0f, 768.0f );
	bool m_bUseCurvature = false;
	Vector2 m_curvatureRange = Vector2( -1.0f, 1.0f );
	bool m_bUseConcavity = false;
	Vector2 m_concavityRange = Vector2( 0.0f, 1.0f );
	bool m_bUseAspect = false;
	float m_fAspectCenter = 0.0f;
	float m_fAspectWidth = 360.0f;
	bool m_bUseFlow = false;
	Vector2 m_flowRange = Vector2( 0.0f, 1.0f );
	bool m_bUseDeposition = false;
	Vector2 m_depositionRange = Vector2( 0.0f, 1.0f );
	bool m_bUseWaterDistance = false;
	Vector2 m_waterDistanceRange = Vector2( 0.0f, 100000.0f );
	bool m_bUseNoise = false;
	Vector2 m_noiseRange = Vector2( 0.0f, 1.0f );
	bool m_bUseBiome = false;
	Vector2 m_biomeRange = Vector2( 0.0f, 1.0f );
	bool m_bUseExistingMaterial = false;
	int m_nExistingBase = -1;
	int m_nExistingOverlay = -1;
} t3dRuleConfig_t;

typedef struct t3dRegionWork_s {
	Vector2i m_location;
	bool m_bAvailable = false;
	bool m_bSuccess = false;
	bool m_bChanged = false;
	int64_t m_nChangedPixels = 0;
	int64_t m_nSkippedManual = 0;
	PackedByteArray m_controlBytes;
	PackedByteArray m_outputBytes;
} t3dRegionWork_t;

typedef struct t3dClearRegionWork_s {
	Vector2i m_location;
	PackedByteArray m_outputBytes;
	int64_t m_nChangedPixels = 0;
	bool m_bChanged = false;
} t3dClearRegionWork_t;

EA_FORCE_INLINE static bool t3dAutoshaderBakerRulePriorityGreater( const t3dRuleConfig_t &rA, const t3dRuleConfig_t &rB ) {
	return rA.m_nPriority > rB.m_nPriority;
}

typedef struct t3dMetricNeeds_s {
	bool m_bSlope = false;
	bool m_bCurvature = false;
	bool m_bConcavity = false;
	bool m_bAspect = false;
	bool m_bFlow = false;
	bool m_bDeposition = false;
	bool m_bWaterDistance = false;
	bool m_bNoise = false;
	bool m_bBiome = false;
} t3dMetricNeeds_t;

typedef struct t3dMetricBuildConfig_s {
	float m_fCurvatureScale = 1.0f;
	float m_fWaterLevel = 0.0f;
	float m_fNoiseScale = 0.01f;
	int m_nNoiseOctaves = 4;
	float m_fNoiseLacunarity = 2.0f;
	float m_fNoiseGain = 0.5f;
	uint32_t m_nNoiseSeed = 1337u;
} t3dMetricBuildConfig_t;

static t3dMetricBuildConfig_t t3dAutoshaderBakerParseMetricBuildConfig( const Dictionary &rProfile ) {
	t3dMetricBuildConfig_t config;
	config.m_fCurvatureScale = MAX( ( float )( rProfile.get( "curvature_scale", 1.0f ) ), 0.0001f );
	config.m_fWaterLevel = ( float )( rProfile.get( "water_level", 0.0f ) );
	config.m_fNoiseScale = MAX( ( float )( rProfile.get( "noise_scale", 0.01f ) ), 0.000001f );
	config.m_nNoiseOctaves = CLAMP( ( int )( rProfile.get( "noise_octaves", 4 ) ), 1, 8 );
	config.m_fNoiseLacunarity = MAX( ( float )( rProfile.get( "noise_lacunarity", 2.0f ) ), 1.0f );
	config.m_fNoiseGain = CLAMP( ( float )( rProfile.get( "noise_gain", 0.5f ) ), 0.0f, 1.0f );
	config.m_nNoiseSeed = ( uint32_t )( ( int64_t )( rProfile.get( "noise_seed", 1337 ) ) );
	return config;
}

typedef struct t3dRegionMetrics_s {
	Terrain3DArenaBuffer<float> m_height;
	Terrain3DArenaBuffer<float> m_heightHalo;
	Terrain3DArenaBuffer<float> m_slope;
	Terrain3DArenaBuffer<float> m_curvature;
	Terrain3DArenaBuffer<float> m_concavity;
	Terrain3DArenaBuffer<float> m_aspect;
	Terrain3DArenaBuffer<float> m_flow;
	Terrain3DArenaBuffer<float> m_deposition;
	Terrain3DArenaBuffer<float> m_waterDistance;
	Terrain3DArenaBuffer<float> m_noise;
	Terrain3DArenaBuffer<float> m_biome;
	Terrain3DArenaBuffer<int32_t> m_flowOrder;
	Terrain3DArenaBuffer<uint32_t> m_outputControl;
} t3dRegionMetrics_t;

typedef struct t3dFlowOrderHeightGreater_s {
	const float *m_pHeight = NULL;

	bool operator()( const int32_t nA, const int32_t nB ) const {
		return m_pHeight[( size_t )( nA )] > m_pHeight[( size_t )( nB )];
	}
} t3dFlowOrderHeightGreater_t;

EA_FORCE_INLINE static Vector2 t3dAutoshaderBakerDictionaryRange(
        const Dictionary &rDictionary, const StringName &rName, const Vector2 &vDefault ) {
	const Variant value = rDictionary.get( rName, vDefault );
	return value.get_type() == Variant::VECTOR2 ? Vector2( value ) : vDefault;
}

static t3dMaskSource_t t3dAutoshaderBakerParseMask( const Dictionary &rProfile, const StringName &rPrefix ) {
	t3dMaskSource_t result;
	const String prefix = String( rPrefix );
	const Variant image_value = rProfile.get( prefix + String( "_image" ), Variant() );
	if ( image_value.get_type() == Variant::OBJECT ) {
		result.m_image = image_value;
	}
	const Variant rect_value = rProfile.get( prefix + String( "_world_rect" ), Rect2() );
	if ( rect_value.get_type() == Variant::RECT2 ) {
		result.m_worldRect = rect_value;
	}
	result.m_nChannel = CLAMP( ( int )( rProfile.get( prefix + String( "_channel" ), 0 ) ), 0, 3 );
	result.m_bInvert = ( bool )( rProfile.get( prefix + String( "_invert" ), false ) );
	if ( result.m_image.is_valid() && !result.m_image->is_empty() ) {
		Ref<Image> pNormalized = Ref<Image>( result.m_image->duplicate() );
		if ( pNormalized.is_valid() ) {
			pNormalized->convert( Image::FORMAT_RGBAF );
			result.m_nWidth = pNormalized->get_width();
			result.m_nHeight = pNormalized->get_height();
			result.m_rgbaFloatData = pNormalized->get_data();
			const size_t expected = ( size_t )( result.m_nWidth ) * ( size_t )( result.m_nHeight ) * 4u * sizeof( float );
			if ( ( size_t )( result.m_rgbaFloatData.size() ) < expected ) {
				result.m_rgbaFloatData.clear();
				result.m_nWidth = 0;
				result.m_nHeight = 0;
			}
		}
	}
	return result;
}

static float t3dAutoshaderBakerSampleMask( const t3dMaskSource_t &rMask, const float fWorldX, const float fWorldZ, const float fDefault ) {
	if ( !rMask.valid() ) {
		return fDefault;
	}
	const float fU = ( fWorldX - rMask.m_worldRect.position.x ) / rMask.m_worldRect.size.x;
	const float fV = ( fWorldZ - rMask.m_worldRect.position.y ) / rMask.m_worldRect.size.y;
	if ( fU < 0.0f || fU > 1.0f || fV < 0.0f || fV > 1.0f ) {
		return fDefault;
	}
	const int nWidth = rMask.m_nWidth;
	const int nHeight = rMask.m_nHeight;
	const int x = CLAMP( ( int )( Math::round( fU * ( float )( nWidth - 1 ) ) ), 0, nWidth - 1 );
	const int y = CLAMP( ( int )( Math::round( fV * ( float )( nHeight - 1 ) ) ), 0, nHeight - 1 );
	float fValue = 0.0f;
	const size_t component = ( ( size_t )( y ) * ( size_t )( nWidth ) + ( size_t )( x ) ) * 4u + ( size_t )( rMask.m_nChannel );
	memcpy( &fValue, rMask.m_rgbaFloatData.ptr() + component * sizeof( float ), sizeof( float ) );
	if ( rMask.m_bInvert ) {
		fValue = 1.0f - fValue;
	}
	return CLAMP( fValue, 0.0f, 1.0f );
}


static float t3dAutoshaderBakerApplyRuleCurve( const t3dRuleConfig_t &rRule, const float fValue ) {
	const float value = CLAMP( fValue, 0.0f, 1.0f );
	if ( rRule.m_curveLut.size() >= 2u ) {
		const float fPosition = value * ( float )( rRule.m_curveLut.size() - 1u );
		const size_t lower = ( size_t )( CLAMP( ( int )( Math::floor( fPosition ) ), 0, ( int )( rRule.m_curveLut.size() - 1u ) ) );
		const size_t upper = MIN( lower + 1u, rRule.m_curveLut.size() - 1u );
		return CLAMP( Math::lerp( rRule.m_curveLut[lower], rRule.m_curveLut[upper], fPosition - ( float )( lower ) ), 0.0f, 1.0f );
	}
	switch ( rRule.m_nCurveMode ) {
		case 0:
			return value;
		case 2:
			return t3dAutoshaderSmootherCurve( value );
		default:
			return t3dAutoshaderSmoothCurve( value );
	}
}

static t3dRuleConfig_t t3dAutoshaderBakerParseRule( const Dictionary &rDictionary, const int nOrder ) {
	t3dRuleConfig_t rule;
	rule.m_bEnabled = ( bool )( rDictionary.get( "enabled", true ) );
	rule.m_nPriority = ( int )( rDictionary.get( "priority", 0 ) );
	rule.m_nOrder = nOrder;
	rule.m_nBaseTexture = ( uint8_t )( CLAMP( ( int )( rDictionary.get( "base_texture", 0 ) ), 0, 31 ) );
	rule.m_nOverlayTexture = ( uint8_t )( CLAMP( ( int )( rDictionary.get( "overlay_texture", 0 ) ), 0, 31 ) );
	rule.m_fBlend = CLAMP( ( float )( rDictionary.get( "blend", 0.0f ) ), 0.0f, 1.0f );
	const float fLegacyFalloff = MAX( ( float )( rDictionary.get( "falloff", 0.0f ) ), 0.0f );
	rule.m_fSlopeFalloff = MAX( ( float )( rDictionary.get( "slope_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fHeightFalloff = MAX( ( float )( rDictionary.get( "height_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fCurvatureFalloff = MAX( ( float )( rDictionary.get( "curvature_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fConcavityFalloff = MAX( ( float )( rDictionary.get( "concavity_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fAspectFalloff = MAX( ( float )( rDictionary.get( "aspect_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fFlowFalloff = MAX( ( float )( rDictionary.get( "flow_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fDepositionFalloff = MAX( ( float )( rDictionary.get( "deposition_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fWaterDistanceFalloff = MAX( ( float )( rDictionary.get( "water_distance_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fNoiseFalloff = MAX( ( float )( rDictionary.get( "noise_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_fBiomeFalloff = MAX( ( float )( rDictionary.get( "biome_falloff", fLegacyFalloff ) ), 0.0f );
	rule.m_bOverwriteManual = ( bool )( rDictionary.get( "overwrite_manual", false ) );
	rule.m_nCurveMode = CLAMP( ( int )( rDictionary.get( "curve_mode", 1 ) ), 0, 2 );
	const Variant curve_value = rDictionary.get( "curve_lut", PackedFloat32Array() );
	if ( curve_value.get_type() == Variant::PACKED_FLOAT32_ARRAY ) {
		const PackedFloat32Array source = curve_value;
		rule.m_curveLut.resize( ( size_t )( source.size() ) );
		for ( int index = 0; index < source.size(); ++index ) {
			rule.m_curveLut[( size_t )( index )] = source[index];
		}
	}

	rule.m_bUseSlope = ( bool )( rDictionary.get( "use_slope", false ) );
	rule.m_slopeRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "slope_range", rule.m_slopeRange );
	rule.m_bUseHeight = ( bool )( rDictionary.get( "use_height", false ) );
	rule.m_heightRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "height_range", rule.m_heightRange );
	rule.m_bUseCurvature = ( bool )( rDictionary.get( "use_curvature", false ) );
	rule.m_curvatureRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "curvature_range", rule.m_curvatureRange );
	rule.m_bUseConcavity = ( bool )( rDictionary.get( "use_concavity", false ) );
	rule.m_concavityRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "concavity_range", rule.m_concavityRange );
	rule.m_bUseAspect = ( bool )( rDictionary.get( "use_aspect", false ) );
	rule.m_fAspectCenter = ( float )( rDictionary.get( "aspect_center", 0.0f ) );
	rule.m_fAspectWidth = CLAMP( ( float )( rDictionary.get( "aspect_width", 360.0f ) ), 0.0f, 360.0f );
	rule.m_bUseFlow = ( bool )( rDictionary.get( "use_flow", false ) );
	rule.m_flowRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "flow_range", rule.m_flowRange );
	rule.m_bUseDeposition = ( bool )( rDictionary.get( "use_deposition", false ) );
	rule.m_depositionRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "deposition_range", rule.m_depositionRange );
	rule.m_bUseWaterDistance = ( bool )( rDictionary.get( "use_water_distance", false ) );
	rule.m_waterDistanceRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "water_distance_range", rule.m_waterDistanceRange );
	rule.m_bUseNoise = ( bool )( rDictionary.get( "use_noise", false ) );
	rule.m_noiseRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "noise_range", rule.m_noiseRange );
	rule.m_bUseBiome = ( bool )( rDictionary.get( "use_biome", false ) );
	rule.m_biomeRange = t3dAutoshaderBakerDictionaryRange( rDictionary, "biome_range", rule.m_biomeRange );
	rule.m_bUseExistingMaterial = ( bool )( rDictionary.get( "use_existing_material", false ) );
	rule.m_nExistingBase = CLAMP( ( int )( rDictionary.get( "existing_base", -1 ) ), -1, 31 );
	rule.m_nExistingOverlay = CLAMP( ( int )( rDictionary.get( "existing_overlay", -1 ) ), -1, 31 );
	return rule;
}

static t3dMetricNeeds_t t3dAutoshaderBakerGetMetricNeeds( const eastl::vector<t3dRuleConfig_t> &rRules ) {
	t3dMetricNeeds_t needs;
	for ( size_t iRule = 0u; iRule < ( size_t )( ( rRules ).size() ); iRule++ ) {
		const t3dRuleConfig_t &rule = ( rRules )[iRule];
		if ( !rule.m_bEnabled ) {
			continue;
		}
		needs.m_bSlope |= rule.m_bUseSlope || rule.m_bUseDeposition;
		needs.m_bCurvature |= rule.m_bUseCurvature || rule.m_bUseConcavity || rule.m_bUseDeposition;
		needs.m_bConcavity |= rule.m_bUseConcavity || rule.m_bUseDeposition;
		needs.m_bAspect |= rule.m_bUseAspect;
		needs.m_bFlow |= rule.m_bUseFlow || rule.m_bUseDeposition;
		needs.m_bDeposition |= rule.m_bUseDeposition;
		needs.m_bWaterDistance |= rule.m_bUseWaterDistance;
		needs.m_bNoise |= rule.m_bUseNoise;
		needs.m_bBiome |= rule.m_bUseBiome;
	}
	return needs;
}

EA_FORCE_INLINE static bool t3dAutoshaderBakerAddMetricLayout(
        Terrain3DLinearArenaLayout &rLayout, const bool bEnabled, const size_t nCount ) {
	return !bEnabled || rLayout.addArray<float>( nCount );
}

EA_FORCE_INLINE static bool t3dAutoshaderBakerAllocateMetric(
        Terrain3DArenaBuffer<float> &fBuffer, Terrain3DLinearArena &rArena, const bool bEnabled, const size_t nCount ) {
	return !bEnabled || fBuffer.allocateUninitialized( rArena, nCount );
}

static void t3dAutoshaderBakerComputeChamferDistance( float *pValues, const int nSize, const float fSpacing ) {
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
					fValue = MIN( fValue, pValues[index - nSize - 1] + fSpacing * s_fSqrtTwo );
				}
				if ( x + 1 < nSize ) {
					fValue = MIN( fValue, pValues[index - nSize + 1] + fSpacing * s_fSqrtTwo );
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
					fValue = MIN( fValue, pValues[index + nSize - 1] + fSpacing * s_fSqrtTwo );
				}
				if ( x + 1 < nSize ) {
					fValue = MIN( fValue, pValues[index + nSize + 1] + fSpacing * s_fSqrtTwo );
				}
			}
			pValues[index] = fValue;
		}
	}
}

static bool t3dAutoshaderBakerMetricArenaBytes( const int nSize, const t3dMetricNeeds_t &rNeeds, size_t &rBytes ) {
	if ( nSize <= 0 ) {
		rBytes = 0u;
		return false;
	}
	const size_t count = ( size_t )( nSize ) * ( size_t )( nSize );
	const bool bNeedsNeighborhood = rNeeds.m_bSlope || rNeeds.m_bCurvature || rNeeds.m_bConcavity || rNeeds.m_bAspect;
	const size_t halo_pitch = ( size_t )( nSize + 2 );
	const size_t halo_count = halo_pitch * halo_pitch;
	Terrain3DLinearArenaLayout layout;
	if ( !layout.addArray<float>( count ) || ( bNeedsNeighborhood && !layout.addArray<float>( halo_count ) )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bSlope, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bCurvature, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bConcavity, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bAspect, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bFlow, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bDeposition, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bWaterDistance, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bNoise, count )
	        || !t3dAutoshaderBakerAddMetricLayout( layout, rNeeds.m_bBiome, count )
	        || ( rNeeds.m_bFlow && !layout.addArray<int32_t>( count ) ) || !layout.addArray<uint32_t>( count ) || !layout.valid() ) {
		rBytes = 0u;
		return false;
	}
	rBytes = layout.size();
	return true;
}

static bool t3dAutoshaderBakerBuildMetrics( Terrain3DData *pData,
        const Vector2i &rLocation,
        const t3dMetricNeeds_t &rNeeds,
        const t3dMetricBuildConfig_t &rConfig,
        const t3dMaskSource_t &rBiomeMask,
        const t3dMaskSource_t &rFlowMask,
        const t3dMaskSource_t &rDepositionMask,
        const t3dMaskSource_t &rWaterMask,
        Terrain3DLinearArena &rArena,
        t3dRegionMetrics_t &rMetrics ) {
	const int size = s_nRegionSize;
	const size_t count = ( size_t )( size ) * ( size_t )( size );
	const bool bNeedsNeighborhood = rNeeds.m_bSlope || rNeeds.m_bCurvature || rNeeds.m_bConcavity || rNeeds.m_bAspect;
	const int halo_pitch = size + 2;
	const size_t halo_count = ( size_t )( halo_pitch ) * ( size_t )( halo_pitch );
	size_t required_arena_bytes = 0u;
	if ( !t3dAutoshaderBakerMetricArenaBytes( size, rNeeds, required_arena_bytes ) || rArena.capacity() < required_arena_bytes ) {
		return false;
	}
	rArena.reset();
	if ( !rMetrics.m_height.allocateUninitialized( rArena, count )
	        || ( bNeedsNeighborhood && !rMetrics.m_heightHalo.allocateUninitialized( rArena, halo_count ) )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_slope, rArena, rNeeds.m_bSlope, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_curvature, rArena, rNeeds.m_bCurvature, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_concavity, rArena, rNeeds.m_bConcavity, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_aspect, rArena, rNeeds.m_bAspect, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_flow, rArena, rNeeds.m_bFlow, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_deposition, rArena, rNeeds.m_bDeposition, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_waterDistance, rArena, rNeeds.m_bWaterDistance, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_noise, rArena, rNeeds.m_bNoise, count )
	        || !t3dAutoshaderBakerAllocateMetric( rMetrics.m_biome, rArena, rNeeds.m_bBiome, count )
	        || ( rNeeds.m_bFlow && !rMetrics.m_flowOrder.allocateUninitialized( rArena, count ) )
	        || !rMetrics.m_outputControl.allocateUninitialized( rArena, count ) ) {
		return false;
	}

	Terrain3DSampler sampler( pData->getSamplingSourceInternal() );
	const int64_t origin_x = ( int64_t )( rLocation.x ) * ( int64_t )( size );
	const int64_t origin_z = ( int64_t )( rLocation.y ) * ( int64_t )( size );
	const float fSpacing = ( float )( pData->getVertexSpacingInternal() );
	const float curvature_scale = rConfig.m_fCurvatureScale;
	const float fWaterLevel = rConfig.m_fWaterLevel;
	const float fNoiseScale = rConfig.m_fNoiseScale;
	const int noise_octaves = rConfig.m_nNoiseOctaves;
	const float noise_lacunarity = rConfig.m_fNoiseLacunarity;
	const float noise_gain = rConfig.m_fNoiseGain;
	const uint32_t noise_seed = rConfig.m_nNoiseSeed;
	if ( bNeedsNeighborhood ) {
		sampler.copyHeightRect( origin_x - 1, origin_z - 1, halo_pitch, halo_pitch, rMetrics.m_heightHalo.data(), halo_pitch, false );
	} else {
		sampler.copyHeightRect( origin_x, origin_z, size, size, rMetrics.m_height.data(), size, false );
	}

	for ( int y = 0; y < size; ++y ) {
		const float *pHaloRow =
		        bNeedsNeighborhood ? rMetrics.m_heightHalo.data() + ( size_t )( y + 1 ) * ( size_t )( halo_pitch ) + 1u : NULL;
		for ( int x = 0; x < size; ++x ) {
			const size_t index = ( size_t )( y ) * ( size_t )( size ) + ( size_t )( x );
			const int64_t grid_x = origin_x + x;
			const int64_t grid_z = origin_z + y;
			float fCenter = bNeedsNeighborhood ? pHaloRow[x] : rMetrics.m_height[index];
			if ( !isfinite( fCenter ) ) {
				fCenter = 0.0f;
			}
			rMetrics.m_height[index] = fCenter;
			const float fWorldX = ( float )( grid_x )*fSpacing;
			const float fWorldZ = ( float )( grid_z )*fSpacing;
			if ( rNeeds.m_bNoise ) {
				rMetrics.m_noise[index] = t3dAutoshaderFbmNoise(
				        fWorldX * fNoiseScale, fWorldZ * fNoiseScale, noise_seed, noise_octaves, noise_lacunarity, noise_gain );
			}
			if ( rNeeds.m_bBiome ) {
				rMetrics.m_biome[index] = t3dAutoshaderBakerSampleMask( rBiomeMask, fWorldX, fWorldZ, 0.0f );
			}
		}
	}

	if ( bNeedsNeighborhood ) {
		for ( int y = 0; y < size; ++y ) {
			const float *pHaloUp = rMetrics.m_heightHalo.data() + ( size_t )( y ) * ( size_t )( halo_pitch );
			const float *pHaloCenter = pHaloUp + halo_pitch;
			const float *pHaloDown = pHaloCenter + halo_pitch;
			for ( int x = 0; x < size; ++x ) {
				const size_t index = ( size_t )( y ) * ( size_t )( size ) + ( size_t )( x );
				const float fCenter = rMetrics.m_height[index];
				const int halo_x = x + 1;
				const float fLeftValue = pHaloCenter[halo_x - 1];
				const float fRightValue = pHaloCenter[halo_x + 1];
				const float fUpValue = pHaloUp[halo_x];
				const float fDownValue = pHaloDown[halo_x];
				const float fLeft = isfinite( fLeftValue ) ? fLeftValue : fCenter;
				const float fRight = isfinite( fRightValue ) ? fRightValue : fCenter;
				const float fUp = isfinite( fUpValue ) ? fUpValue : fCenter;
				const float fDown = isfinite( fDownValue ) ? fDownValue : fCenter;
				const float fDx = ( fRight - fLeft ) / ( 2.0f * fSpacing );
				const float fDz = ( fDown - fUp ) / ( 2.0f * fSpacing );
				if ( rNeeds.m_bSlope ) {
					rMetrics.m_slope[index] = Math::atan( Math::sqrt( fDx * fDx + fDz * fDz ) ) * s_fDegreesPerRadian;
				}
				if ( rNeeds.m_bAspect ) {
					float fAspect = Math::atan2( -fDx, -fDz ) * s_fDegreesPerRadian;
					if ( fAspect < 0.0f ) {
						fAspect += 360.0f;
					}
					rMetrics.m_aspect[index] = fAspect;
				}
				if ( rNeeds.m_bCurvature || rNeeds.m_bConcavity ) {
					const float fCurvature = CLAMP( ( ( fLeft + fRight + fUp + fDown ) * 0.25f - fCenter ) / curvature_scale, -1.0f, 1.0f );
					if ( rNeeds.m_bCurvature ) {
						rMetrics.m_curvature[index] = fCurvature;
					}
					if ( rNeeds.m_bConcavity ) {
						rMetrics.m_concavity[index] = MAX( fCurvature, 0.0f );
					}
				}
			}
		}
	}

	if ( rNeeds.m_bFlow ) {
		if ( rFlowMask.valid() ) {
			for ( int y = 0; y < size; ++y ) {
				for ( int x = 0; x < size; ++x ) {
					const size_t index = ( size_t )( y ) * ( size_t )( size ) + ( size_t )( x );
					const float fWorldX = ( float )( origin_x + x ) * fSpacing;
					const float fWorldZ = ( float )( origin_z + y ) * fSpacing;
					rMetrics.m_flow[index] = t3dAutoshaderBakerSampleMask( rFlowMask, fWorldX, fWorldZ, 0.0f );
				}
			}
		} else {
			for ( size_t index = 0; index < count; ++index ) {
				rMetrics.m_flow[index] = 1.0f;
				rMetrics.m_flowOrder[index] = ( int32_t )( index );
			}
			eastl::sort(
			        rMetrics.m_flowOrder.begin(), rMetrics.m_flowOrder.end(), t3dFlowOrderHeightGreater_t{ rMetrics.m_height.data() } );
			float fMaximumFlow = 1.0f;
			for ( size_t order_index = 0; order_index < count; ++order_index ) {
				const int index = rMetrics.m_flowOrder[order_index];
				const int x = index % size;
				const int y = index / size;
				const float fSourceHeight = rMetrics.m_height[( size_t )( index )];
				int destination = -1;
				float fDestinationHeight = fSourceHeight;
				for ( int offset_y = -1; offset_y <= 1; ++offset_y ) {
					for ( int offset_x = -1; offset_x <= 1; ++offset_x ) {
						if ( ( offset_x == 0 && offset_y == 0 ) || x + offset_x < 0 || x + offset_x >= size || y + offset_y < 0
						        || y + offset_y >= size ) {
							continue;
						}
						const int neighbor = ( y + offset_y ) * size + x + offset_x;
						const float fNeighborHeight = rMetrics.m_height[( size_t )( neighbor )];
						if ( fNeighborHeight < fDestinationHeight ) {
							fDestinationHeight = fNeighborHeight;
							destination = neighbor;
						}
					}
				}
				if ( destination >= 0 ) {
					rMetrics.m_flow[( size_t )( destination )] += rMetrics.m_flow[( size_t )( index )];
					fMaximumFlow = MAX( fMaximumFlow, rMetrics.m_flow[( size_t )( destination )] );
				}
			}
			const float fDenominator = Math::log( 1.0f + fMaximumFlow );
			for ( size_t index = 0; index < count; ++index ) {
				rMetrics.m_flow[index] = fDenominator > 0.0f ? Math::log( 1.0f + rMetrics.m_flow[index] ) / fDenominator : 0.0f;
			}
		}
	}

	if ( rNeeds.m_bDeposition ) {
		for ( int y = 0; y < size; ++y ) {
			for ( int x = 0; x < size; ++x ) {
				const size_t index = ( size_t )( y ) * ( size_t )( size ) + ( size_t )( x );
				const float fWorldX = ( float )( origin_x + x ) * fSpacing;
				const float fWorldZ = ( float )( origin_z + y ) * fSpacing;
				if ( rDepositionMask.valid() ) {
					rMetrics.m_deposition[index] = t3dAutoshaderBakerSampleMask( rDepositionMask, fWorldX, fWorldZ, 0.0f );
				} else {
					const float fFlatness = 1.0f - CLAMP( rMetrics.m_slope[index] / 90.0f, 0.0f, 1.0f );
					rMetrics.m_deposition[index] = CLAMP( rMetrics.m_concavity[index] * fFlatness * rMetrics.m_flow[index], 0.0f, 1.0f );
				}
			}
		}
	}

	if ( rNeeds.m_bWaterDistance ) {
		for ( int y = 0; y < size; ++y ) {
			for ( int x = 0; x < size; ++x ) {
				const size_t index = ( size_t )( y ) * ( size_t )( size ) + ( size_t )( x );
				const float fWorldX = ( float )( origin_x + x ) * fSpacing;
				const float fWorldZ = ( float )( origin_z + y ) * fSpacing;
				const bool bWater = rWaterMask.valid() ? t3dAutoshaderBakerSampleMask( rWaterMask, fWorldX, fWorldZ, 0.0f ) >= 0.5f
				                                       : rMetrics.m_height[index] <= fWaterLevel;
				rMetrics.m_waterDistance[index] = bWater ? 0.0f : s_fLargeDistance;
			}
		}
		t3dAutoshaderBakerComputeChamferDistance( rMetrics.m_waterDistance.data(), size, fSpacing );
	}
	return rArena.used() == rArena.capacity();
}

static float t3dAutoshaderBakerEvaluateRule( const t3dRuleConfig_t &rRule,
        const t3dRegionMetrics_t &rMetrics,
        const size_t iIndex,
        const uint32_t nControl,
        const bool bAllowManual ) {
	if ( !rRule.m_bEnabled || ( !t3dIsAuto( nControl ) && !bAllowManual ) ) {
		return 0.0f;
	}
	if ( rRule.m_bUseExistingMaterial
	        && ( ( rRule.m_nExistingBase >= 0 && ( int )( t3dGetBase( nControl ) ) != rRule.m_nExistingBase )
	                || ( rRule.m_nExistingOverlay >= 0 && ( int )( t3dGetOverlay( nControl ) ) != rRule.m_nExistingOverlay ) ) ) {
		return 0.0f;
	}
	float fScore = 1.0f;
	if ( rRule.m_bUseSlope ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight( rMetrics.m_slope[iIndex], rRule.m_slopeRange.x, rRule.m_slopeRange.y, rRule.m_fSlopeFalloff ) );
	}
	if ( rRule.m_bUseHeight ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight(
		                rMetrics.m_height[iIndex], rRule.m_heightRange.x, rRule.m_heightRange.y, rRule.m_fHeightFalloff ) );
	}
	if ( rRule.m_bUseCurvature ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight(
		                rMetrics.m_curvature[iIndex], rRule.m_curvatureRange.x, rRule.m_curvatureRange.y, rRule.m_fCurvatureFalloff ) );
	}
	if ( rRule.m_bUseConcavity ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight(
		                rMetrics.m_concavity[iIndex], rRule.m_concavityRange.x, rRule.m_concavityRange.y, rRule.m_fConcavityFalloff ) );
	}
	if ( rRule.m_bUseAspect ) {
		const float fHalfWidth = rRule.m_fAspectWidth * 0.5f;
		const float fDistance = t3dAutoshaderAngleDistanceDegrees( rMetrics.m_aspect[iIndex], rRule.m_fAspectCenter );
		fScore = MIN( fScore, t3dAutoshaderRangeWeight( fDistance, 0.0f, fHalfWidth, rRule.m_fAspectFalloff ) );
	}
	if ( rRule.m_bUseFlow ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight( rMetrics.m_flow[iIndex], rRule.m_flowRange.x, rRule.m_flowRange.y, rRule.m_fFlowFalloff ) );
	}
	if ( rRule.m_bUseDeposition ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight(
		                rMetrics.m_deposition[iIndex], rRule.m_depositionRange.x, rRule.m_depositionRange.y, rRule.m_fDepositionFalloff ) );
	}
	if ( rRule.m_bUseWaterDistance ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight( rMetrics.m_waterDistance[iIndex],
		                rRule.m_waterDistanceRange.x,
		                rRule.m_waterDistanceRange.y,
		                rRule.m_fWaterDistanceFalloff ) );
	}
	if ( rRule.m_bUseNoise ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight( rMetrics.m_noise[iIndex], rRule.m_noiseRange.x, rRule.m_noiseRange.y, rRule.m_fNoiseFalloff ) );
	}
	if ( rRule.m_bUseBiome ) {
		fScore = MIN( fScore,
		        t3dAutoshaderRangeWeight( rMetrics.m_biome[iIndex], rRule.m_biomeRange.x, rRule.m_biomeRange.y, rRule.m_fBiomeFalloff ) );
	}
	return t3dAutoshaderBakerApplyRuleCurve( rRule, fScore );
}

static uint64_t t3dAutoshaderBakerFnv1aBytes( const uint8_t *pBytes, const size_t nSize, uint64_t nHash = 1469598103934665603ull ) {
	for ( size_t index = 0; index < nSize; ++index ) {
		nHash ^= ( uint64_t )( pBytes[index] );
		nHash *= 1099511628211ull;
	}
	return nHash;
}

static uint64_t t3dAutoshaderBakerRegionSignature( Terrain3DRegion *pRegion, const uint64_t nProfileSignature ) {
	uint64_t result = nProfileSignature;
	if ( !pRegion ) {
		return result;
	}
	const Ref<Image> pHeight = pRegion->getHeightMap();
	if ( pHeight.is_valid() ) {
		const PackedByteArray bytes = pHeight->get_data();
		result = t3dAutoshaderBakerFnv1aBytes( bytes.ptr(), ( size_t )( bytes.size() ), result );
	}

	const int region_size = s_nRegionSize;
	const size_t pixel_count = ( size_t )( region_size ) * ( size_t )( region_size );
	const Ref<Image> pControlImage = pRegion->getControlMap();
	PackedByteArray control_bytes;
	const uint8_t *pControlRead = NULL;
	if ( pControlImage.is_valid() ) {
		control_bytes = pControlImage->get_data();
		if ( ( size_t )( control_bytes.size() ) >= pixel_count * sizeof( uint32_t ) ) {
			pControlRead = control_bytes.ptr();
		}
	}
	for ( size_t index = 0; index < pixel_count; ++index ) {
		uint32_t control = s_nRegionDefaultControl;
		if ( pControlRead ) {
			memcpy( &control, pControlRead + index * sizeof( uint32_t ), sizeof( uint32_t ) );
		}
		// Baked material fields are outputs, so exclude them from auto-pixel
		// provenance. Manual pixels remain exact inputs for overwrite/filter rules.
		const uint32_t source_control = t3dIsAuto( control ) ? control & ~s_nAutoshaderMaterialMask : control;
		result = t3dAutoshaderBakerFnv1aBytes( ( const uint8_t * )( &source_control ), sizeof( source_control ), result );
	}
	return result;
}

static TypedArray<Vector2i> t3dAutoshaderBakerNormalizeLocations( Terrain3DData *pData, const TypedArray<Vector2i> &rLocations ) {
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
	Terrain3DData *m_pData = NULL;
	eastl::vector<t3dRegionWork_t> *m_pWork = NULL;
	const eastl::vector<t3dRuleConfig_t> *m_pRules = NULL;
	const t3dMetricNeeds_t *m_pNeeds = NULL;
	const t3dMetricBuildConfig_t *m_pMetricConfig = NULL;
	const t3dMaskSource_t *m_pBiomeMask = NULL;
	const t3dMaskSource_t *m_pFlowMask = NULL;
	const t3dMaskSource_t *m_pDepositionMask = NULL;
	const t3dMaskSource_t *m_pWaterMask = NULL;
	size_t m_nPixelCount = 0u;
	bool m_bOnlyAutoPixels = true;
	bool m_bPreserveAutoMask = true;
	uint8_t m_nDefaultBase = 0u;
	uint8_t m_nDefaultOverlay = 0u;
	uint8_t m_nDefaultBlend = 0u;
	Terrain3DLinearArena *m_pWorkerArenas = NULL;
} t3dAutoShaderRegionContext_t;

static void t3dAutoshaderBakerBakeAutoshaderRegions( void *pContext, const size_t nBegin, const size_t nEnd, const int32_t nWorker ) {
	t3dAutoShaderRegionContext_t *context = ( t3dAutoShaderRegionContext_t * )( pContext );
	for ( size_t work_index = nBegin; work_index < nEnd; ++work_index ) {
		t3dRegionWork_t &item = ( *context->m_pWork )[work_index];
		if ( !item.m_bAvailable ) {
			continue;
		}
		Terrain3DLinearArena &arena = context->m_pWorkerArenas[MAX( nWorker, 0 )];
		t3dRegionMetrics_t metrics;
		if ( !t3dAutoshaderBakerBuildMetrics( context->m_pData,
		             item.m_location,
		             *context->m_pNeeds,
		             *context->m_pMetricConfig,
		             *context->m_pBiomeMask,
		             *context->m_pFlowMask,
		             *context->m_pDepositionMask,
		             *context->m_pWaterMask,
		             arena,
		             metrics ) ) {
			continue;
		}
		item.m_bSuccess = true;
		const uint8_t *pControlRead = item.m_controlBytes.is_empty() ? NULL : item.m_controlBytes.ptr();
		for ( size_t index = 0; index < context->m_nPixelCount; ++index ) {
			uint32_t control = s_nRegionDefaultControl;
			if ( pControlRead ) {
				memcpy( &control, pControlRead + index * sizeof( uint32_t ), sizeof( uint32_t ) );
			}
			metrics.m_outputControl[index] = control;
			const bool bAutoPixel = t3dIsAuto( control );
			const t3dRuleConfig_t *pMatchedRule = NULL;
			float fMatchedScore = 0.0f;
			for ( size_t iRule = 0u; iRule < ( size_t )( ( *context->m_pRules ).size() ); iRule++ ) {
				const t3dRuleConfig_t &rule = ( *context->m_pRules )[iRule];
				const bool bAllowManual =
				        t3dAutoshaderRuleCanProcessPixel( bAutoPixel, context->m_bOnlyAutoPixels, rule.m_bOverwriteManual );
				const float fScore = t3dAutoshaderBakerEvaluateRule( rule, metrics, index, control, bAllowManual );
				if ( fScore > 0.0f ) {
					pMatchedRule = &rule;
					fMatchedScore = fScore;
					break;
				}
			}
			if ( !bAutoPixel && context->m_bOnlyAutoPixels && !pMatchedRule ) {
				++item.m_nSkippedManual;
				continue;
			}
			uint8_t base = context->m_nDefaultBase;
			uint8_t overlay = context->m_nDefaultOverlay;
			uint8_t blend = context->m_nDefaultBlend;
			if ( pMatchedRule ) {
				base = pMatchedRule->m_nBaseTexture;
				overlay = pMatchedRule->m_nOverlayTexture;
				blend = ( uint8_t )( CLAMP( ( int )( Math::round( pMatchedRule->m_fBlend * fMatchedScore * s_fByteMaxFloat ) ), 0, 255 ) );
			}
			if ( base == overlay ) {
				blend = 0u;
			}
			uint32_t replacement =
			        ( control & ~s_nAutoshaderMaterialMask ) | t3dEncBase( base ) | t3dEncOverlay( overlay ) | t3dEncBlend( blend );
			if ( context->m_bPreserveAutoMask ) {
				replacement = ( replacement & ~0x1u ) | t3dEncAuto( t3dIsAuto( control ) );
			} else {
				replacement &= ~0x1u;
			}
			if ( replacement != control ) {
				metrics.m_outputControl[index] = replacement;
				item.m_bChanged = true;
				++item.m_nChangedPixels;
			}
		}
		if ( item.m_bChanged ) {
			item.m_outputBytes.resize( ( int64_t )( context->m_nPixelCount * sizeof( uint32_t ) ) );
			memcpy( item.m_outputBytes.ptrw(), metrics.m_outputControl.data(), context->m_nPixelCount * sizeof( uint32_t ) );
		}
	}
}

Dictionary Terrain3DAutoShaderBaker::bakeRegions(
        Terrain3D *const pTerrain, const Dictionary &rProfile, const TypedArray<Vector2i> &rRegionLocations ) {
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

	Terrain3DData *const data = pTerrain->getData();
	const TypedArray<Vector2i> locations = t3dAutoshaderBakerNormalizeLocations( data, rRegionLocations );
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
	rules.reserve( ( size_t )( rule_array.size() ) );
	for ( int index = 0; index < rule_array.size(); ++index ) {
		if ( rule_array[index].get_type() == Variant::DICTIONARY ) {
			rules.push_back( t3dAutoshaderBakerParseRule( rule_array[index], index ) );
		}
	}
	eastl::stable_sort( rules.begin(), rules.end(), t3dAutoshaderBakerRulePriorityGreater );
	const t3dMetricNeeds_t needs = t3dAutoshaderBakerGetMetricNeeds( rules );
	const bool only_auto_pixels = ( bool )( rProfile.get( "only_auto_pixels", true ) );
	const bool preserve_auto_mask = ( bool )( rProfile.get( "preserve_auto_mask", true ) );
	const uint8_t default_base = ( uint8_t )( CLAMP( ( int )( rProfile.get( "default_base_texture", 0 ) ), 0, 31 ) );
	const uint8_t default_overlay = ( uint8_t )( CLAMP( ( int )( rProfile.get( "default_overlay_texture", 0 ) ), 0, 31 ) );
	const uint8_t default_blend =
	        ( uint8_t )( CLAMP( ( int )( Math::round( ( float )( rProfile.get( "default_blend", 0.0f ) ) * s_fByteMaxFloat ) ), 0, 255 ) );
	const Ref<Terrain3DAssets> pAssets = pTerrain->getAssets();
	if ( pAssets.is_null() ) {
		result["error"] = "Terrain3D has no texture asset registry.";
		return result;
	}
	if ( !t3dAutoshaderBakerTextureSlotIsPopulated( pAssets, default_base )
	        || !t3dAutoshaderBakerTextureSlotIsPopulated( pAssets, default_overlay ) ) {
		result["error"] = "The autoshader profile fallback references a missing terrain texture.";
		return result;
	}
	for ( size_t iRule = 0u; iRule < ( size_t )( ( rules ).size() ); iRule++ ) {
		const t3dRuleConfig_t &rule = ( rules )[iRule];
		if ( !rule.m_bEnabled ) {
			continue;
		}
		if ( !t3dAutoshaderBakerTextureSlotIsPopulated( pAssets, rule.m_nBaseTexture )
		        || !t3dAutoshaderBakerTextureSlotIsPopulated( pAssets, rule.m_nOverlayTexture ) ) {
			result["error"] = String( "Material rule " ) + itos( rule.m_nOrder + 1 ) + String( " references a missing terrain texture." );
			return result;
		}
	}
	const uint64_t profile_signature = ( uint64_t )( ( int64_t )( rProfile.get( "profile_signature", 0 ) ) );
	const t3dMaskSource_t biome_mask = t3dAutoshaderBakerParseMask( rProfile, "biome_mask" );
	const t3dMaskSource_t flow_mask = t3dAutoshaderBakerParseMask( rProfile, "flow_mask" );
	const t3dMaskSource_t deposition_mask = t3dAutoshaderBakerParseMask( rProfile, "deposition_mask" );
	const t3dMaskSource_t water_mask = t3dAutoshaderBakerParseMask( rProfile, "water_mask" );
	const t3dMetricBuildConfig_t metric_config = t3dAutoshaderBakerParseMetricBuildConfig( rProfile );

	eastl::vector<Vector2i> changed_locations;
	changed_locations.reserve( ( size_t )( locations.size() ) );
	Dictionary signatures;
	PackedStringArray errors;
	int64_t changed_pixel_count = 0;
	int64_t skipped_manual_count = 0;
	int processed_region_count = 0;

	const int size = s_nRegionSize;
	const size_t count = ( size_t )( size ) * ( size_t )( size );
	eastl::vector<t3dRegionWork_t> work( ( size_t )( locations.size() ) );
	for ( int location_index = 0; location_index < locations.size(); ++location_index ) {
		t3dRegionWork_t &item = work[( size_t )( location_index )];
		item.m_location = locations[location_index];
		if ( !data->ensureCpuRegionResident( item.m_location ) ) {
			continue;
		}
		Terrain3DRegion *const pRegion = data->getRegionPtr( item.m_location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		item.m_bAvailable = true;
		const Ref<Image> pControlImage = pRegion->getControlMap();
		if ( pControlImage.is_valid() ) {
			item.m_controlBytes = pControlImage->get_data();
			if ( ( size_t )item.m_controlBytes.size() < count * sizeof( uint32_t ) ) {
				item.m_controlBytes.clear();
			}
		}
	}

	const uint32_t hardware_threads = MAX( 1u, ( uint32_t )( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	const size_t worker_count = MAX<size_t>( 1u, MIN<size_t>( ( size_t )hardware_threads, work.size() ) );
	const Terrain3DParallelExecutor executor( ( int32_t )worker_count );
	size_t worker_arena_bytes = 0u;
	if ( !t3dAutoshaderBakerMetricArenaBytes( size, needs, worker_arena_bytes ) ) {
		result["error"] = "Could not calculate fixed autoshader worker scratch.";
		return result;
	}
	void *const pWorkerArenaBacking = t3dMemoryAllocate( worker_count * sizeof( Terrain3DLinearArena ) );
	if ( !pWorkerArenaBacking ) {
		result["error"] = "Could not allocate fixed autoshader worker arena descriptors.";
		return result;
	}
	Terrain3DLinearArena *const pWorkerArenas = ( Terrain3DLinearArena * )( pWorkerArenaBacking );
	size_t configured_worker_count = 0u;
	for ( ; configured_worker_count < worker_count; ++configured_worker_count ) {
		new ( pWorkerArenas + configured_worker_count ) Terrain3DLinearArena();
		if ( !pWorkerArenas[configured_worker_count].configure( worker_arena_bytes ) ) {
			++configured_worker_count;
			for ( size_t iWorker = 0u; iWorker < configured_worker_count; ++iWorker ) {
				pWorkerArenas[iWorker].~Terrain3DLinearArena();
			}
			t3dMemoryRelease( pWorkerArenaBacking );
			result["error"] = "Could not establish fixed autoshader worker scratch.";
			return result;
		}
	}
	t3dAutoShaderRegionContext_t bake_context;
	bake_context.m_pData = data;
	bake_context.m_pWork = &work;
	bake_context.m_pRules = &rules;
	bake_context.m_pNeeds = &needs;
	bake_context.m_pMetricConfig = &metric_config;
	bake_context.m_pBiomeMask = &biome_mask;
	bake_context.m_pFlowMask = &flow_mask;
	bake_context.m_pDepositionMask = &deposition_mask;
	bake_context.m_pWaterMask = &water_mask;
	bake_context.m_nPixelCount = count;
	bake_context.m_bOnlyAutoPixels = only_auto_pixels;
	bake_context.m_bPreserveAutoMask = preserve_auto_mask;
	bake_context.m_nDefaultBase = default_base;
	bake_context.m_nDefaultOverlay = default_overlay;
	bake_context.m_nDefaultBlend = default_blend;
	bake_context.m_pWorkerArenas = pWorkerArenas;
	executor.runStatic( work.size(), 1u, t3dAutoshaderBakerBakeAutoshaderRegions, &bake_context );
	for ( size_t iWorker = 0u; iWorker < worker_count; ++iWorker ) {
		pWorkerArenas[iWorker].~Terrain3DLinearArena();
	}
	t3dMemoryRelease( pWorkerArenaBacking );

	// Validate the complete batch before mutating region resources. Editor callers
	// can therefore treat ok=false as "nothing was retained" without rebuilding a
	// second rollback path around partial native commits.
	for ( size_t iWork = 0u; iWork < ( size_t )( ( work ).size() ); iWork++ ) {
		t3dRegionWork_t &item = ( work )[iWork];
		if ( !data->ensureCpuRegionResident( item.m_location ) ) {
			errors.push_back( String( "Could not page CPU terrain maps for region " ) + String( item.m_location ) + String( "." ) );
			continue;
		}
		Terrain3DRegion *const pRegion = data->getRegionPtr( item.m_location );
		if ( !item.m_bAvailable || !pRegion || pRegion->isDeleted() ) {
			errors.push_back( String( "Region " ) + String( item.m_location ) + String( " is unavailable." ) );
			continue;
		}
		if ( !item.m_bSuccess ) {
			errors.push_back( String( "Could not allocate or build metrics for region " ) + String( item.m_location ) + String( "." ) );
			continue;
		}
		++processed_region_count;
		changed_pixel_count += item.m_nChangedPixels;
		skipped_manual_count += item.m_nSkippedManual;
	}
	if ( !errors.is_empty() ) {
		result["processed_regions"] = processed_region_count;
		result["skipped_manual_pixels"] = skipped_manual_count;
		result["errors"] = errors;
		return result;
	}

	for ( size_t iWork = 0u; iWork < ( size_t )( ( work ).size() ); iWork++ ) {
		t3dRegionWork_t &item = ( work )[iWork];
		Terrain3DRegion *const pRegion = data->getRegionPtr( item.m_location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( item.m_bChanged ) {
			const Ref<Image> pOutput = Image::create_from_data( size, size, false, Image::FORMAT_RF, item.m_outputBytes );
			pRegion->setControlMap( pOutput );
			changed_locations.push_back( item.m_location );
		}
		signatures[Terrain3DUtil::locationToString( item.m_location )] =
		        ( int64_t )( t3dAutoshaderBakerRegionSignature( pRegion, profile_signature ) );
	}

	if ( !changed_locations.empty() ) {
		data->commitMapEdits(
		        TYPE_CONTROL, changed_locations, AABB(), MAP_EDIT_SNAP | MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_UPDATE_COLLISION );
	}
	result["ok"] = true;
	result["processed_regions"] = processed_region_count;
	result["changed_regions"] = ( int )( changed_locations.size() );
	result["changed_pixels"] = changed_pixel_count;
	result["skipped_manual_pixels"] = skipped_manual_count;
	result["errors"] = errors;
	result["region_signatures"] = signatures;
	return result;
}

Dictionary Terrain3DAutoShaderBaker::computeRegionSignatures(
        Terrain3D *const pTerrain, const Dictionary &rProfile, const TypedArray<Vector2i> &rRegionLocations ) {
	Dictionary signatures;
	if ( !pTerrain || !pTerrain->getData() ) {
		return signatures;
	}
	Terrain3DData *const pData = pTerrain->getData();
	const TypedArray<Vector2i> locations = t3dAutoshaderBakerNormalizeLocations( pData, rRegionLocations );
	const uint64_t profile_signature = ( uint64_t )( ( int64_t )( rProfile.get( "profile_signature", 0 ) ) );
	for ( int index = 0; index < locations.size(); ++index ) {
		const Vector2i location = locations[index];
		if ( !pData->ensureCpuRegionResident( location ) ) {
			continue;
		}
		Terrain3DRegion *const pRegion = pData->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		signatures[Terrain3DUtil::locationToString( location )] =
		        ( int64_t )( t3dAutoshaderBakerRegionSignature( pRegion, profile_signature ) );
	}
	return signatures;
}

Dictionary Terrain3DAutoShaderBaker::clearBakedRegions(
        Terrain3D *const pTerrain, const TypedArray<Vector2i> &rRegionLocations, const bool bPreserveAutoMask ) {
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
	Terrain3DData *const pData = pTerrain->getData();
	const TypedArray<Vector2i> locations = t3dAutoshaderBakerNormalizeLocations( pData, rRegionLocations );
	eastl::vector<t3dClearRegionWork_t> work;
	work.reserve( ( size_t )( locations.size() ) );
	PackedStringArray errors;
	for ( int location_index = 0; location_index < locations.size(); ++location_index ) {
		const Vector2i location = locations[location_index];
		if ( !pData->ensureCpuRegionResident( location ) ) {
			errors.push_back( String( "Could not page CPU terrain maps for region " ) + String( location ) + String( "." ) );
			continue;
		}
		Terrain3DRegion *const pRegion = pData->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			errors.push_back( String( "Region " ) + String( location ) + String( " is unavailable." ) );
			continue;
		}
		if ( pRegion->getControlMap().is_null() ) {
			continue;
		}
		const int size = s_nRegionSize;
		const size_t count = ( size_t )( size ) * ( size_t )( size );
		PackedByteArray bytes = pRegion->getControlMap()->get_data();
		if ( pRegion->getControlMap()->get_format() != Image::FORMAT_RF || ( size_t )( bytes.size() ) < count * sizeof( uint32_t ) ) {
			errors.push_back( String( "Region " ) + String( location ) + String( " has an invalid control map." ) );
			continue;
		}
		uint8_t *const pWrite = bytes.ptrw();
		t3dClearRegionWork_t item;
		item.m_location = location;
		for ( size_t index = 0; index < count; ++index ) {
			uint32_t control = 0u;
			memcpy( &control, pWrite + index * sizeof( uint32_t ), sizeof( uint32_t ) );
			if ( !t3dIsAuto( control ) ) {
				continue;
			}
			uint32_t replacement = control & ~s_nAutoshaderMaterialMask;
			replacement = ( replacement & ~0x1u ) | t3dEncAuto( true );
			if ( replacement != control ) {
				memcpy( pWrite + index * sizeof( uint32_t ), &replacement, sizeof( uint32_t ) );
				item.m_bChanged = true;
				++item.m_nChangedPixels;
			}
		}
		if ( item.m_bChanged ) {
			item.m_outputBytes = bytes;
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
	for ( size_t iWork = 0u; iWork < ( size_t )( ( work ).size() ); iWork++ ) {
		t3dClearRegionWork_t &item = ( work )[iWork];
		Terrain3DRegion *const pRegion = pData->getRegionPtr( item.m_location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const int size = s_nRegionSize;
		pRegion->setControlMap( Image::create_from_data( size, size, false, Image::FORMAT_RF, item.m_outputBytes ) );
		changed_locations.push_back( item.m_location );
		changed_pixels += item.m_nChangedPixels;
	}
	if ( !changed_locations.empty() ) {
		pData->commitMapEdits(
		        TYPE_CONTROL, changed_locations, AABB(), MAP_EDIT_SNAP | MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_UPDATE_COLLISION );
	}
	result["ok"] = true;
	result["changed_regions"] = ( int )( changed_locations.size() );
	result["changed_pixels"] = changed_pixels;
	return result;
}

void Terrain3DAutoShaderBaker::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "bake_regions", "terrain", "profile", "region_locations" ), &Terrain3DAutoShaderBaker::bakeRegions );
	ClassDB::bind_method( D_METHOD( "compute_region_signatures", "terrain", "profile", "region_locations" ),
	        &Terrain3DAutoShaderBaker::computeRegionSignatures );
	ClassDB::bind_method( D_METHOD( "clear_baked_regions", "terrain", "region_locations", "preserve_auto_mask" ),
	        &Terrain3DAutoShaderBaker::clearBakedRegions,
	        DEFVAL( true ) );
}
