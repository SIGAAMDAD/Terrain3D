// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/material/terrain_3d_shader_specializer.h"

#include "integration/godot/terrain_3d_binding.h"
#include "core/utility/terrain_3d_generation.h"
#include "Terrain3DTypeSupport.h"
#include "Terrain3DConstants.h"
#include "rendering/material/terrain_3d_material.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/material/terrain_3d_material_schema.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"

#include "core/utility/logger.h"
#include "core/utility/terrain_3d_owner.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "data/model/terrain_3d_data.h"
#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "integration/godot/terrain_3d_resource_persistence.h"

#include <string.h>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/texture.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

t3dShaderVariantState_t Terrain3DMaterial::captureVariantState(
	void ) const {
	t3dShaderVariantState_t state;
	state.m_worldBackground = m_authoringState.m_worldBackground;
	state.m_projectionMode = m_authoringState.m_projectionMode;
	state.m_tessellationMaterialMode = m_authoringState.m_tessellationMaterialMode;
	state.m_worldNoiseShaderMode = m_authoringState.m_worldNoiseShaderMode;
	state.m_bAutoShaderEnabled = m_authoringState.m_bAutoShaderEnabled;
	state.m_bDualScalingEnabled = m_authoringState.m_bDualScalingEnabled;
	state.m_bMacroVariationEnabled = m_authoringState.m_bMacroVariationEnabled;
	state.m_bOutputAlbedoEnabled = m_authoringState.m_bOutputAlbedoEnabled;
	state.m_bOutputRoughnessEnabled = m_authoringState.m_bOutputRoughnessEnabled;
	state.m_bOutputSpecularEnabled = m_authoringState.m_bOutputSpecularEnabled;
	state.m_bOutputNormalMapEnabled = m_authoringState.m_bOutputNormalMapEnabled;
	state.m_bOutputAmbientOcclusionEnabled = m_authoringState.m_bOutputAmbientOcclusionEnabled;
	state.m_bFastBlendCurve = m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_RUNTIME_360 ||
							  m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_XBOX_360_AUTHENTIC ||
							  m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_LOW ||
							  m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_MINIMUM;
	state.m_bTessellationEnabled = m_pTerrain && m_pTerrain->getTessellationLevel() > 0;
	const Ref< Terrain3DAssets > pAssets = m_pTerrain ? m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
	state.m_bSurfacePacked = pAssets.is_valid() && pAssets->usesPackedPbr();
	Terrain3DData *const pData = m_pTerrain ? m_pTerrain->dataObject() : NULL;
	state.m_bControlMapsImplicit = pData && pData->areControlMapsImplicit();
	state.m_bColorMapsImplicit = pData && !pData->isGpuRegionColorChannelEnabled();
	applyTierQualityToState( state, m_authoringState.m_nearQuality, SHADER_TIER_NEAR );
	return state;
}

void Terrain3DMaterial::applyXboxProfileManifestToState(
	t3dShaderVariantState_t &rState ) const {
	for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
		const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
		if ( setting.m_owner != T3D_PROFILE_OWNER_MATERIAL ) {
			continue;
		}
		const char *property = setting.m_pszProperty;
		if ( strcmp( property, "projection_mode" ) == 0 ) {
			rState.m_projectionMode = ( t3dProjectionMode_t ) ( setting.m_nIntValue );
		} else if ( strcmp( property, "tessellation_material_mode" ) == 0 ) {
			rState.m_tessellationMaterialMode = ( t3dTessellationMaterialMode_t ) ( setting.m_nIntValue );
		} else if ( strcmp( property, "world_noise_shader_mode" ) == 0 ) {
			rState.m_worldNoiseShaderMode = ( t3dWorldNoiseShaderMode_t ) ( setting.m_nIntValue );
		} else if ( strcmp( property, "auto_shader_enabled" ) == 0 ) {
			rState.m_bAutoShaderEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "dual_scaling_enabled" ) == 0 ) {
			rState.m_bDualScalingEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "macro_variation_enabled" ) == 0 ) {
			rState.m_bMacroVariationEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_albedo" ) == 0 ) {
			rState.m_bOutputAlbedoEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_roughness" ) == 0 ) {
			rState.m_bOutputRoughnessEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_specular" ) == 0 ) {
			rState.m_bOutputSpecularEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_normal_map" ) == 0 ) {
			rState.m_bOutputNormalMapEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_ambient_occlusion" ) == 0 ) {
			rState.m_bOutputAmbientOcclusionEnabled = setting.m_bBoolValue;
		}
	}
}

void Terrain3DMaterial::applyXboxProfileManifestToTiers(
	void ) {
	for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
		const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
		if ( setting.m_owner != T3D_PROFILE_OWNER_MATERIAL ) {
			continue;
		}
		t3dTierQualityConfig_t *pQuality = NULL;
		const char			   *suffix = NULL;
		if ( strncmp( setting.m_pszProperty, "near_", 5 ) == 0 ) {
			pQuality = &m_authoringState.m_nearQuality;
			suffix = setting.m_pszProperty + 5;
		} else if ( strncmp( setting.m_pszProperty, "middle_", 7 ) == 0 ) {
			pQuality = &m_authoringState.m_middleQuality;
			suffix = setting.m_pszProperty + 7;
		} else if ( strncmp( setting.m_pszProperty, "far_", 4 ) == 0 ) {
			pQuality = &m_authoringState.m_farQuality;
			suffix = setting.m_pszProperty + 4;
		} else {
			continue;
		}

		if ( strcmp( suffix, "texture_filtering" ) == 0 ) {
			pQuality->m_textureFiltering = ( t3dTextureFiltering_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "material_sampling_quality" ) == 0 ) {
			pQuality->m_materialSamplingQuality = ( t3dMaterialSamplingQuality_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "terrain_normal_quality" ) == 0 ) {
			pQuality->m_terrainNormalQuality = ( t3dTerrainNormalQuality_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "terrain_color_quality" ) == 0 ) {
			pQuality->m_terrainColorQuality = ( t3dTerrainColorQuality_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "surface_response_quality" ) == 0 ) {
			pQuality->m_surfaceResponseQuality = ( t3dSurfaceResponseQuality_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "texture_surface_mode" ) == 0 ) {
			pQuality->m_textureSurfaceMode = ( t3dTextureSurfaceMode_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "lighting_model" ) == 0 ) {
			pQuality->m_lightingModel = ( t3dTierLightingModel_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "detiling_quality" ) == 0 ) {
			pQuality->m_detilingQuality = ( t3dTierDetilingQuality_t ) ( setting.m_nIntValue );
		} else if ( strcmp( suffix, "authored_color_contribution" ) == 0 ) {
			pQuality->m_fAuthoredColorContribution = ( real_t ) ( setting.m_fFloatValue );
		} else if ( strcmp( suffix, "wetness_contribution" ) == 0 ) {
			pQuality->m_fWetnessContribution = ( real_t ) ( setting.m_fFloatValue );
		} else if ( strcmp( suffix, "material_lod_bias" ) == 0 ) {
			pQuality->m_fMaterialLodBias = ( real_t ) ( setting.m_fFloatValue );
		}
	}
}

void Terrain3DMaterial::applyPerformancePresetToState(
	t3dShaderVariantState_t			  &rState,
	const t3dShaderPerformancePreset_t ePreset ) const {
	switch ( ePreset ) {
	case SHADER_PRESET_CINEMATIC:
		rState.m_textureFiltering = LINEAR_ANISOTROPIC;
		rState.m_terrainSamplingMode = TERRAIN_SAMPLING_ALWAYS_SMOOTH;
		rState.m_terrainNormalMode = TERRAIN_NORMAL_HEIGHT_8_TAP;
		rState.m_colorSamplingMode = COLOR_SAMPLING_EXACT_4_TAP;
		rState.m_projectionMode = PROJECTION_EXACT_PER_CORNER;
		rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT;
		rState.m_surfaceDataMode = SURFACE_DATA_FULL;
		rState.m_terrainShadingMode = SHADING_BURLEY_GGX;
		rState.m_bOutputAlbedoEnabled = true;
		rState.m_bOutputNormalMapEnabled = true;
		rState.m_bOutputRoughnessEnabled = true;
		rState.m_bOutputSpecularEnabled = true;
		rState.m_bOutputAmbientOcclusionEnabled = true;
		rState.m_detilingMode = DETILING_CONTINUOUS;
		rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_FRAGMENT_NORMALS;
		rState.m_bFastBlendCurve = false;
		break;
	case SHADER_PRESET_HIGH:
		rState.m_textureFiltering = LINEAR_ANISOTROPIC;
		rState.m_terrainSamplingMode = TERRAIN_SAMPLING_ADAPTIVE;
		rState.m_terrainNormalMode = TERRAIN_NORMAL_DERIVED_TEXTURE;
		rState.m_colorSamplingMode = COLOR_SAMPLING_AUTO;
		rState.m_projectionMode = PROJECTION_SHARED_FRAGMENT;
		rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
		rState.m_surfaceDataMode = SURFACE_DATA_AUTO;
		rState.m_terrainShadingMode = SHADING_BURLEY_GGX;
		rState.m_bOutputAlbedoEnabled = true;
		rState.m_bOutputNormalMapEnabled = true;
		rState.m_bOutputRoughnessEnabled = true;
		rState.m_bOutputSpecularEnabled = true;
		rState.m_bOutputAmbientOcclusionEnabled = true;
		rState.m_detilingMode = DETILING_DISCRETE_8;
		rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
		rState.m_bFastBlendCurve = false;
		break;
	case SHADER_PRESET_RUNTIME_360:
		// Stylized/performative 360 mode: modern cached normals and discrete
		// detiling, but no expensive PBR specular or multi-scale layers.
		rState.m_textureFiltering = LINEAR;
		rState.m_terrainSamplingMode = TERRAIN_SAMPLING_SINGLE_SAMPLE;
		rState.m_terrainNormalMode = TERRAIN_NORMAL_DERIVED_TEXTURE;
		rState.m_colorSamplingMode = COLOR_SAMPLING_HARDWARE_FILTERED;
		rState.m_projectionMode = PROJECTION_DISABLED;
		rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
		rState.m_surfaceDataMode = SURFACE_DATA_AUTO;
		rState.m_terrainShadingMode = SHADING_LAMBERT_DIFFUSE_ONLY;
		rState.m_bOutputAlbedoEnabled = true;
		rState.m_bOutputNormalMapEnabled = true;
		rState.m_bOutputRoughnessEnabled = false;
		rState.m_bOutputSpecularEnabled = false;
		rState.m_bOutputAmbientOcclusionEnabled = true;
		rState.m_detilingMode = DETILING_DISCRETE_8;
		rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
		rState.m_bAutoShaderEnabled = false;
		rState.m_bDualScalingEnabled = false;
		rState.m_bMacroVariationEnabled = false;
		rState.m_bFastBlendCurve = true;
		break;
	case SHADER_PRESET_LOW:
		rState.m_textureFiltering = LINEAR;
		rState.m_terrainSamplingMode = TERRAIN_SAMPLING_SINGLE_SAMPLE;
		rState.m_terrainNormalMode = TERRAIN_NORMAL_HEIGHT_3_TAP;
		rState.m_colorSamplingMode = COLOR_SAMPLING_HARDWARE_FILTERED;
		rState.m_projectionMode = PROJECTION_DISABLED;
		rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
		rState.m_surfaceDataMode = SURFACE_DATA_AUTO;
		rState.m_terrainShadingMode = SHADING_LAMBERT_DIFFUSE_ONLY;
		rState.m_bOutputAlbedoEnabled = true;
		rState.m_bOutputNormalMapEnabled = true;
		rState.m_bOutputRoughnessEnabled = false;
		rState.m_bOutputSpecularEnabled = false;
		rState.m_bOutputAmbientOcclusionEnabled = true;
		rState.m_detilingMode = DETILING_DISABLED;
		rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
		rState.m_bFastBlendCurve = true;
		break;
	case SHADER_PRESET_MINIMUM:
		rState.m_textureFiltering = LINEAR;
		rState.m_terrainSamplingMode = TERRAIN_SAMPLING_SINGLE_SAMPLE;
		rState.m_terrainNormalMode = TERRAIN_NORMAL_GEOMETRY_DERIVATIVE;
		rState.m_colorSamplingMode = COLOR_SAMPLING_HARDWARE_FILTERED;
		rState.m_projectionMode = PROJECTION_DISABLED;
		rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
		rState.m_surfaceDataMode = SURFACE_DATA_ALBEDO_ONLY;
		rState.m_terrainShadingMode = SHADING_UNSHADED;
		rState.m_bOutputAlbedoEnabled = true;
		rState.m_bOutputNormalMapEnabled = false;
		rState.m_bOutputRoughnessEnabled = false;
		rState.m_bOutputSpecularEnabled = false;
		rState.m_bOutputAmbientOcclusionEnabled = false;
		rState.m_detilingMode = DETILING_DISABLED;
		rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
		rState.m_bFastBlendCurve = true;
		break;
	case SHADER_PRESET_XBOX_360_AUTHENTIC:
		applyXboxProfileManifestToState( rState );
		rState.m_bFastBlendCurve = true;
		break;
	case SHADER_PRESET_CUSTOM:
	default:
		break;
	}
}

void Terrain3DMaterial::applyPerformancePresetToTiers(
	const t3dShaderPerformancePreset_t ePreset ) {
	// Presets seed the same semantic rows for all tiers. The authentic Xbox
	// profile reads its owned values from the authoritative native manifest;
	// the other material-only presets retain their local definitions.
	m_authoringState.m_middleQuality.m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	m_authoringState.m_middleQuality.m_fAuthoredColorContribution = 1.0f;
	m_authoringState.m_farQuality.m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	m_authoringState.m_farQuality.m_fAuthoredColorContribution = 1.0f;

	switch ( ePreset ) {
	case SHADER_PRESET_CINEMATIC:
		m_authoringState.m_middleQuality.m_textureFiltering = LINEAR_ANISOTROPIC;
		m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
		m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
		m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
		m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
		m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_PBR;
		m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_ROTATION;
		m_authoringState.m_middleQuality.m_fWetnessContribution = 1.0f;
		m_authoringState.m_middleQuality.m_fMaterialLodBias = -0.5f;
		m_authoringState.m_farQuality.m_textureFiltering = LINEAR_ANISOTROPIC;
		m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
		m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
		m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
		m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
		m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_PBR;
		m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_ROTATION;
		m_authoringState.m_farQuality.m_fWetnessContribution = 1.0f;
		m_authoringState.m_farQuality.m_fMaterialLodBias = -1.5f;
		break;
	case SHADER_PRESET_HIGH:
		m_authoringState.m_middleQuality.m_textureFiltering = LINEAR_ANISOTROPIC;
		m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
		m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
		m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
		m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
		m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
		m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_ROTATION;
		m_authoringState.m_middleQuality.m_fWetnessContribution = 1.0f;
		m_authoringState.m_middleQuality.m_fMaterialLodBias = 0.0f;
		m_authoringState.m_farQuality.m_textureFiltering = LINEAR;
		m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
		m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
		m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
		m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
		m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
		m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_ROTATION;
		m_authoringState.m_farQuality.m_fWetnessContribution = 1.0f;
		m_authoringState.m_farQuality.m_fMaterialLodBias = -0.5f;
		break;
	case SHADER_PRESET_RUNTIME_360:
		m_authoringState.m_middleQuality.m_textureFiltering = LINEAR;
		m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
		m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
		m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
		m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
		m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
		m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_DISABLED;
		m_authoringState.m_middleQuality.m_fWetnessContribution = 1.0f;
		m_authoringState.m_middleQuality.m_fMaterialLodBias = 0.75f;
		m_authoringState.m_farQuality.m_textureFiltering = LINEAR;
		m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
		m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
		m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
		m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
		m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
		m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_DISABLED;
		m_authoringState.m_farQuality.m_fWetnessContribution = 1.0f;
		m_authoringState.m_farQuality.m_fMaterialLodBias = 0.0f;
		break;
	case SHADER_PRESET_XBOX_360_AUTHENTIC:
		applyXboxProfileManifestToTiers();
		break;
	case SHADER_PRESET_LOW:
		m_authoringState.m_middleQuality.m_textureFiltering = LINEAR;
		m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
		m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
		m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
		m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
		m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
		m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_DISABLED;
		m_authoringState.m_middleQuality.m_fWetnessContribution = 0.5f;
		m_authoringState.m_middleQuality.m_fMaterialLodBias = 1.5f;
		m_authoringState.m_farQuality.m_textureFiltering = LINEAR;
		m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
		m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
		m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
		m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
		m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
		m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_DISABLED;
		m_authoringState.m_farQuality.m_fWetnessContribution = 0.0f;
		m_authoringState.m_farQuality.m_fMaterialLodBias = 1.0f;
		break;
	case SHADER_PRESET_MINIMUM:
		m_authoringState.m_middleQuality.m_textureFiltering = NEAREST;
		m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
		m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
		m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
		m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
		m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_UNSHADED;
		m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_DISABLED;
		m_authoringState.m_middleQuality.m_fWetnessContribution = 0.0f;
		m_authoringState.m_middleQuality.m_fMaterialLodBias = 2.5f;
		m_authoringState.m_farQuality.m_textureFiltering = NEAREST;
		m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
		m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
		m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
		m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
		m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_UNSHADED;
		m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_DISABLED;
		m_authoringState.m_farQuality.m_fWetnessContribution = 0.0f;
		m_authoringState.m_farQuality.m_fMaterialLodBias = 2.0f;
		break;
	case SHADER_PRESET_CUSTOM:
	default:
		break;
	}
}

void Terrain3DMaterial::applyVariantState(
	const t3dShaderVariantState_t &eState ) {
	m_authoringState.m_worldBackground = eState.m_worldBackground;
	m_authoringState.m_projectionMode = eState.m_projectionMode;
	m_authoringState.m_tessellationMaterialMode = eState.m_tessellationMaterialMode;
	m_authoringState.m_worldNoiseShaderMode = eState.m_worldNoiseShaderMode;
	m_authoringState.m_bAutoShaderEnabled = eState.m_bAutoShaderEnabled;
	m_authoringState.m_bDualScalingEnabled = eState.m_bDualScalingEnabled;
	m_authoringState.m_bMacroVariationEnabled = eState.m_bMacroVariationEnabled;
	m_authoringState.m_bOutputAlbedoEnabled = eState.m_bOutputAlbedoEnabled;
	m_authoringState.m_bOutputRoughnessEnabled = eState.m_bOutputRoughnessEnabled;
	m_authoringState.m_bOutputSpecularEnabled = eState.m_bOutputSpecularEnabled;
	m_authoringState.m_bOutputNormalMapEnabled = eState.m_bOutputNormalMapEnabled;
	m_authoringState.m_bOutputAmbientOcclusionEnabled = eState.m_bOutputAmbientOcclusionEnabled;
	m_authoringState.m_nearQuality.m_textureFiltering = eState.m_textureFiltering;
	m_authoringState.m_nearQuality.m_materialSamplingQuality =
		eState.m_terrainSamplingMode == TERRAIN_SAMPLING_SINGLE_SAMPLE ? MATERIAL_SAMPLING_FAST
																	   : MATERIAL_SAMPLING_FULL;
	m_authoringState.m_nearQuality.m_terrainNormalQuality =
		eState.m_terrainNormalMode == TERRAIN_NORMAL_GEOMETRY_DERIVATIVE ? TERRAIN_NORMAL_QUALITY_GEOMETRY
																		 : TERRAIN_NORMAL_QUALITY_DETAILED;
	m_authoringState.m_nearQuality.m_terrainColorQuality = eState.m_colorSamplingMode == COLOR_SAMPLING_EXACT_4_TAP
															   ? TERRAIN_COLOR_QUALITY_EXACT
															   : TERRAIN_COLOR_QUALITY_FAST;
	m_authoringState.m_nearQuality.m_surfaceResponseQuality =
		eState.m_surfaceDataMode == SURFACE_DATA_ALBEDO_ONLY ? SURFACE_RESPONSE_SIMPLE : SURFACE_RESPONSE_DETAILED;
	m_authoringState.m_nearQuality.m_textureSurfaceMode = eState.m_textureSurfaceMode;
	m_authoringState.m_nearQuality.m_lightingModel =
		eState.m_terrainShadingMode == SHADING_UNSHADED
			? TIER_LIGHTING_UNSHADED
			: ( eState.m_terrainShadingMode == SHADING_XBOX_360_BLINN_PHONG
					? TIER_LIGHTING_BLINN_PHONG
					: ( eState.m_terrainShadingMode == SHADING_BURLEY_GGX ||
								eState.m_terrainShadingMode == SHADING_LAMBERT_GGX
							? TIER_LIGHTING_PBR
							: TIER_LIGHTING_LAMBERT ) );
	m_authoringState.m_nearQuality.m_detilingQuality =
		eState.m_detilingMode == DETILING_DISABLED ? TIER_DETILING_DISABLED : TIER_DETILING_ROTATION;
}

void Terrain3DMaterial::applyTierQualityToState(
	t3dShaderVariantState_t		 &rState,
	const t3dTierQualityConfig_t &rQuality,
	const t3dShaderTier_t		  tier ) const {
	rState.m_textureFiltering = rQuality.m_textureFiltering;
	rState.m_terrainSamplingMode = rQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST
									   ? TERRAIN_SAMPLING_SINGLE_SAMPLE
									   : TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED;
	rState.m_terrainNormalMode = rQuality.m_terrainNormalQuality == TERRAIN_NORMAL_QUALITY_GEOMETRY
									 ? TERRAIN_NORMAL_GEOMETRY_DERIVATIVE
									 : TERRAIN_NORMAL_DERIVED_TEXTURE;
	rState.m_colorSamplingMode = rQuality.m_terrainColorQuality == TERRAIN_COLOR_QUALITY_EXACT
									 ? COLOR_SAMPLING_EXACT_4_TAP
									 : COLOR_SAMPLING_HARDWARE_FILTERED;
	rState.m_surfaceDataMode =
		rQuality.m_surfaceResponseQuality == SURFACE_RESPONSE_DETAILED ? SURFACE_DATA_FULL : SURFACE_DATA_ALBEDO_ONLY;
	rState.m_textureSurfaceMode = rQuality.m_textureSurfaceMode;
	rState.m_terrainShadingMode =
		rQuality.m_lightingModel == TIER_LIGHTING_UNSHADED
			? SHADING_UNSHADED
			: ( rQuality.m_lightingModel == TIER_LIGHTING_BLINN_PHONG
					? SHADING_XBOX_360_BLINN_PHONG
					: ( rQuality.m_lightingModel == TIER_LIGHTING_PBR ? SHADING_BURLEY_GGX
																	  : SHADING_LAMBERT_DIFFUSE_ONLY ) );
	rState.m_detilingMode =
		rQuality.m_detilingQuality == TIER_DETILING_DISABLED ? DETILING_DISABLED : DETILING_DISCRETE_8;
	// Auto-material classification is shared across quality tiers. Far rendering does
	// not consume the detailed control-map autoshader path.
	if ( tier == SHADER_TIER_FAR ) {
		rState.m_bAutoShaderEnabled = false;
	}
	rState.m_bDualScalingEnabled = false;
	rState.m_bMacroVariationEnabled = false;
	if ( tier != SHADER_TIER_NEAR ) {
		rState.m_bOutputAlbedoEnabled = true;
		rState.m_bOutputRoughnessEnabled = false;
		rState.m_bOutputSpecularEnabled = false;
		rState.m_bOutputNormalMapEnabled = false;
		rState.m_bOutputAmbientOcclusionEnabled = false;
	}
}

// Ripped from ShaderPreprocessor::CommentRemover::strip()
static EA_FORCE_INLINE void t3dMaterialMixSignature(
	uint64_t	  &rHash,
	const uint64_t nValue ) {
	rHash ^= nValue;
	rHash *= 1099511628211ULL;
}

void Terrain3DMaterial::bumpRdPublicationGeneration(
	void ) {
	m_runtimeState.m_nRdPublicationGeneration = t3dNextGeneration( m_runtimeState.m_nRdPublicationGeneration );
}

void Terrain3DMaterial::setRuntimeMaterialParam(
	const StringName &strName,
	const Variant	 &rValue ) {
	// The editor cursor is transient overlay state. The UI already filters
	// unchanged values; live Godot Arrays may share storage with the last
	// published Variant, so comparing them here would miss in-place edits.
	// Never invalidate authored material layers, WORLD geometry, HiZ or VT
	// demand when only an editor decal/brush texture changed.
	if ( strName.begins_with( "_editor_" ) ) {
		m_runtimeState.m_runtimeMaterialParams[strName] = rValue;
		m_runtimeState.m_nEditorOverlayGeneration =
			t3dNextGeneration( m_runtimeState.m_nEditorOverlayGeneration );
		return;
	}
	const Variant current = m_runtimeState.m_runtimeMaterialParams.get( strName, Variant() );
	if ( current == rValue ) {
		return;
	}
	m_runtimeState.m_runtimeMaterialParams[strName] = rValue;
	bumpRdPublicationGeneration();
}

void Terrain3DMaterial::refreshNativeMaterialState(
	const bool bRefreshSchema,
	const bool bNotifyPropertyList ) {
	TypedArray< StringName > previous_active_params;
	if ( bRefreshSchema ) {
		previous_active_params = m_runtimeState.m_activeParams;
		refreshMaterialPropertyCache();
		for ( int iParam = 0; iParam < previous_active_params.size(); ++iParam ) {
			const StringName name = previous_active_params[iParam];
			if ( !m_runtimeState.m_activeParams.has( name ) ) {
				setRuntimeMaterialParam( name, Variant() );
			}
		}
	}

	for ( int iParam = 0; iParam < m_runtimeState.m_activeParams.size(); ++iParam ) {
		const StringName name = m_runtimeState.m_activeParams[iParam];
		const Variant	 value =
			m_authoringState.m_shaderParams.get( name, m_authoringState.m_shaderParamDefaults.get( name, Variant() ) );
		if ( value.get_type() == Variant::OBJECT ) {
			const Ref< Texture > texture = value;
			setRuntimeMaterialParam( name, texture.is_valid() ? Variant( texture ) : Variant() );
		} else {
			setRuntimeMaterialParam( name, value );
		}
	}

	PackedVector2Array control_rotations;
	control_rotations.resize( 16 );
	for ( int i = 0; i < 16; ++i ) {
		const real_t angle = ( real_t ) ( i ) * -0.392699081698724;
		control_rotations.set( i, Vector2( Math::cos( angle ), Math::sin( angle ) ) );
	}
	setRuntimeMaterialParam( "_control_rotation_cs", control_rotations );

	PackedVector2Array detile_rotations;
	detile_rotations.resize( 16 );
	for ( int i = 0; i < 16; ++i ) {
		const real_t angle = ( real_t ) ( i ) *Math::TAU / 16.0;
		detile_rotations.set( i, Vector2( Math::cos( angle ), Math::sin( angle ) ) );
	}
	setRuntimeMaterialParam( "_detile_rotation_cs", detile_rotations );

	if ( m_runtimeState.m_activeParams.has( "noise1_angle" ) ) {
		const real_t angle = m_authoringState.m_shaderParams.get(
			StringName( "noise1_angle" ),
			m_authoringState.m_shaderParamDefaults.get( StringName( "noise1_angle" ), ( real_t ) ( 0.0f ) ) );
		setRuntimeMaterialParam( "_noise1_rotation", Vector2( Math::cos( angle ), Math::sin( angle ) ) );
	} else {
		setRuntimeMaterialParam( "_noise1_rotation", Variant() );
	}
	setRuntimeMaterialParam( "_background_mode", m_authoringState.m_worldBackground );

	// Feature/output/debug toggles are not all represented by named parameters.
	// Publish every native mutation even when the dictionary itself is unchanged.
	bumpRdPublicationGeneration();
	if ( bNotifyPropertyList ) {
		notify_property_list_changed();
	}
}

void Terrain3DMaterial::invalidateRegionPageTables(
	void ) {
	m_vtBinding.m_generatedRegionMap.clear();
	m_vtBinding.m_nRegionMapTextureHash = 0u;
	m_vtBinding.m_nRegionMapTextureSize = 0;
	m_vtBinding.m_generatedLogicalRegionMap.clear();
	m_vtBinding.m_nLogicalRegionMapTextureHash = 0u;
	m_vtBinding.m_nLogicalRegionMapTextureSize = 0;
	m_vtBinding.m_nRegionPageTableOwnerGeneration = 0u;
}

bool Terrain3DMaterial::updateRegionMapTexture(
	const PackedInt32Array &rRegionMap,
	const int				nRegionMapSize ) {
	if ( nRegionMapSize <= 0 || nRegionMapSize > T3D_REGION_MAP_SIZE ||
		 rRegionMap.size() != nRegionMapSize * nRegionMapSize ) {
		return false;
	}
	uint64_t hash = 1469598103934665603ULL;
	t3dMaterialMixSignature( hash, ( uint64_t ) ( nRegionMapSize ) );
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		t3dMaterialMixSignature( hash, ( uint64_t ) ( ( uint32_t ) ( rRegionMap[index] ) ) );
	}
	if ( m_vtBinding.m_generatedRegionMap.getRid().is_valid() && m_vtBinding.m_nRegionMapTextureHash == hash &&
		 m_vtBinding.m_nRegionMapTextureSize == nRegionMapSize ) {
		return true;
	}

	PackedByteArray bytes;
	bytes.resize( rRegionMap.size() * ( int ) ( sizeof( uint16_t ) ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		const uint16_t value = ( uint16_t ) ( CLAMP( rRegionMap[index], 0, ( int ) ( UINT16_MAX ) ) );
		memcpy( pWrite + ( size_t ) ( index ) * sizeof( uint16_t ), &value, sizeof( uint16_t ) );
	}
	const Ref< Image > pImage =
		Image::create_from_data( nRegionMapSize, nRegionMapSize, false, Image::FORMAT_R16, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bSameSizeTexture =
		m_vtBinding.m_generatedRegionMap.getRid().is_valid() && m_vtBinding.m_nRegionMapTextureSize == nRegionMapSize;
	bool bUploaded = bSameSizeTexture
						 ? m_vtBinding.m_generatedRegionMap.update2d( pImage )
						 : m_vtBinding.m_generatedRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	if ( !bUploaded && bSameSizeTexture ) {
		// A World3D/owner transition can leave a renderer-side RID unusable even
		// though the cached wrapper still describes the same dimensions. Discard
		// that identity and create a fresh page table instead of treating it as
		// authoritative forever.
		m_vtBinding.m_generatedRegionMap.clear();
		bUploaded = m_vtBinding.m_generatedRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	}
	if ( !bUploaded ) {
		m_vtBinding.m_nRegionMapTextureHash = 0u;
		m_vtBinding.m_nRegionMapTextureSize = 0;
		return false;
	}
	m_vtBinding.m_nRegionMapTextureHash = hash;
	m_vtBinding.m_nRegionMapTextureSize = nRegionMapSize;
	return true;
}

bool Terrain3DMaterial::updateLogicalRegionMapTexture(
	const PackedInt32Array &rRegionMap,
	const int				nRegionMapSize ) {
	if ( nRegionMapSize <= 0 || nRegionMapSize > T3D_REGION_MAP_SIZE ||
		 rRegionMap.size() != nRegionMapSize * nRegionMapSize ) {
		return false;
	}
	uint64_t hash = 1469598103934665603ULL;
	t3dMaterialMixSignature( hash, ( uint64_t ) ( nRegionMapSize ) );
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		t3dMaterialMixSignature( hash, ( uint64_t ) ( ( uint32_t ) ( rRegionMap[index] ) ) );
	}
	if ( m_vtBinding.m_generatedLogicalRegionMap.getRid().is_valid() &&
		 m_vtBinding.m_nLogicalRegionMapTextureHash == hash &&
		 m_vtBinding.m_nLogicalRegionMapTextureSize == nRegionMapSize ) {
		return true;
	}
	PackedByteArray bytes;
	bytes.resize( rRegionMap.size() );
	uint8_t *pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		pWrite[index] = rRegionMap[index] != 0 ? 255u : 0u;
	}
	const Ref< Image > pImage =
		Image::create_from_data( nRegionMapSize, nRegionMapSize, false, Image::FORMAT_R8, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bSameSizeTexture = m_vtBinding.m_generatedLogicalRegionMap.getRid().is_valid() &&
								  m_vtBinding.m_nLogicalRegionMapTextureSize == nRegionMapSize;
	bool bUploaded =
		bSameSizeTexture
			? m_vtBinding.m_generatedLogicalRegionMap.update2d( pImage )
			: m_vtBinding.m_generatedLogicalRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	if ( !bUploaded && bSameSizeTexture ) {
		m_vtBinding.m_generatedLogicalRegionMap.clear();
		bUploaded = m_vtBinding.m_generatedLogicalRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	}
	if ( !bUploaded ) {
		m_vtBinding.m_nLogicalRegionMapTextureHash = 0u;
		m_vtBinding.m_nLogicalRegionMapTextureSize = 0;
		return false;
	}
	m_vtBinding.m_nLogicalRegionMapTextureHash = hash;
	m_vtBinding.m_nLogicalRegionMapTextureSize = nRegionMapSize;
	return true;
}

t3dPageTableBuildResult_t Terrain3DMaterial::prepareRegionPageTables(
	void ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return PAGE_TABLE_INVALID_TERRAIN_STATE;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	if ( !pData->isRuntimeReady() ) {
		return PAGE_TABLE_INVALID_TERRAIN_STATE;
	}
	const uint32_t owner_generation = pData->getActiveStorageGeneration();
	if ( owner_generation == 0u ) {
		return PAGE_TABLE_INVALID_TERRAIN_STATE;
	}
	if ( m_vtBinding.m_nRegionPageTableOwnerGeneration != owner_generation ) {
		invalidateRegionPageTables();
	}

	const PackedInt32Array region_map = pData->getRegionMap();
	const int			   region_map_size = pData->getRegionMapSizeInternal();
	const PackedInt32Array logical_region_map = pData->getLogicalRegionMap();
	if ( region_map_size <= 0 || region_map_size > T3D_REGION_MAP_SIZE ||
		 region_map.size() != region_map_size * region_map_size ||
		 logical_region_map.size() != region_map_size * region_map_size ) {
		return PAGE_TABLE_INVALID_DIMENSIONS;
	}
	if ( !updateRegionMapTexture( region_map, region_map_size ) ) {
		return PAGE_TABLE_TEXTURE_ALLOCATION_FAILED;
	}
	if ( !updateLogicalRegionMapTexture( logical_region_map, region_map_size ) ) {
		return PAGE_TABLE_TEXTURE_ALLOCATION_FAILED;
	}
	m_vtBinding.m_nRegionPageTableOwnerGeneration = owner_generation;
	return PAGE_TABLE_READY;
}

void Terrain3DMaterial::setShaderParameters(
	const Dictionary &rDict ) {
	if ( m_authoringState.m_shaderParams == rDict ) {
		return;
	}
	SET_IF_DIFF( m_authoringState.m_shaderParams, rDict );
	m_authoringState.m_bPersistenceDirty = true;
	refreshNativeMaterialState( false, false );
	LOG( INFO, "Setting shader params dictionary: ", rDict.size() );
}

// -----------------------------------------------------------------------------
// Public functions

// This function serves as the constructor which is initialized by the class Terrain3D.
// Godot likes to create resource objects at startup, so this prevents it from creating
// uninitialized materials.
void Terrain3DMaterial::initialize(
	Terrain3DOwner *pTerrain ) {
	const uint64_t initialize_started_usec = Time::get_singleton()->get_ticks_usec();
	if ( pTerrain ) {
		Terrain3DOwner *previous_owner = t3dTransferOwner( m_pTerrain, pTerrain );
		if ( previous_owner != pTerrain ) {
			invalidateRegionPageTables();
		}
		if ( previous_owner && previous_owner != pTerrain ) {
			LOG( INFO,
				 "Transferring material runtime ownership from ",
				 t3dOwnerDiagnosticIdentity( previous_owner ),
				 " to ",
				 t3dOwnerDiagnosticIdentity( pTerrain ) );
		}
	} else {
		LOG( ERROR, "[Terrain3D][Startup][Material] FAIL reason=null_terrain" );
		return;
	}
	LOG( INFO, "[Terrain3D][Startup][Material] BEGIN" );
	refreshNativeMaterialState( true, false );
	const uint64_t dummy_textures_started_usec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][Material][DummyTextures] BEGIN" );
	{ // Create dummy texture arrays to avoid empty sampler2DArrays
		const Ref< Image >		  pImg = Image::create( 1, 1, false, Image::FORMAT_RF );
		const TypedArray< Image > ia = { pImg };
		m_runtimeState.m_generatedDummy.create( ia, T3D_RD_TEXTURE_LINEAR_DATA );

		const Ref< Image >		  pSurfaceImg = Terrain3DUtil::getDefaultSurfaceImage( V2I( 1 ), false );
		const TypedArray< Image > surface_array = { pSurfaceImg };
		m_runtimeState.m_generatedSurfaceDummy.create( surface_array, T3D_RD_TEXTURE_LINEAR_DATA );
		const Ref< Image >		  pDisplacementImg = Terrain3DUtil::getDefaultDisplacementImage( V2I( 1 ), false );
		const TypedArray< Image > displacement_array = { pDisplacementImg };
		m_runtimeState.m_generatedDisplacementDummy.create( displacement_array, T3D_RD_TEXTURE_LINEAR_DATA );
	}
	LOG( INFO,
		 "[Terrain3D][Startup][Material][DummyTextures] END elapsed=",
		 Time::get_singleton()->get_ticks_usec() - dummy_textures_started_usec,
		 " us" );

	const uint64_t rebuild_started_usec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][Material][FullRebuild] BEGIN" );
	update( FULL_REBUILD );
	LOG( INFO,
		 "[Terrain3D][Startup][Material][FullRebuild] END elapsed=",
		 Time::get_singleton()->get_ticks_usec() - rebuild_started_usec,
		 " us" );
	m_authoringState.m_bPersistenceDirty = false;
	LOG( INFO,
		 "[Terrain3D][Startup][Material] END elapsed=",
		 Time::get_singleton()->get_ticks_usec() - initialize_started_usec,
		 " us" );
}

void Terrain3DMaterial::uninitialize(
	Terrain3DOwner *pTerrain ) {
	if ( !t3dReleaseOwner( m_pTerrain, pTerrain ) ) {
		LOG( DEBUG,
			 "Ignoring stale material uninitialize request from ",
			 t3dOwnerDiagnosticIdentity( pTerrain ),
			 "; current owner: ",
			 t3dOwnerDiagnosticIdentity( m_pTerrain ) );
		return;
	}
	LOG( INFO, "Uninitializing material" );
	invalidateRegionPageTables();
	t3dVectorRelease( m_authoringState.m_shaderPropertyCache );
}

void Terrain3DMaterial::destroy(
	void ) {
	LOG( INFO, "Destroying material" );
	m_pTerrain = NULL;
	m_runtimeState.m_activeParams.clear();
	m_authoringState.m_shaderParams.clear();
	t3dVectorRelease( m_authoringState.m_shaderPropertyCache );
	m_authoringState.m_shaderParamDefaults.clear();
	m_runtimeState.m_runtimeMaterialParams.clear();
	m_runtimeState.m_generatedDummy.clear();
	m_runtimeState.m_generatedSurfaceDummy.clear();
	m_runtimeState.m_generatedDisplacementDummy.clear();
	invalidateRegionPageTables();
}

void Terrain3DMaterial::update(
	const uint32_t nFlags ) {
	const bool bRefreshSchema = ( nFlags & FULL_REBUILD ) != 0u;
	refreshNativeMaterialState( bRefreshSchema, bRefreshSchema );
	const bool bStartupRebuild = bRefreshSchema && m_pTerrain && !m_pTerrain->isInitialized();
	if ( nFlags & REGION_ARRAYS ) {
		const uint64_t page_tables_started_usec = Time::get_singleton()->get_ticks_usec();
		if ( bStartupRebuild ) {
			LOG( INFO, "[Terrain3D][Startup][Material][PageTables] BEGIN" );
		}
		const t3dPageTableBuildResult_t page_table_result = prepareRegionPageTables();
		if ( page_table_result != PAGE_TABLE_READY ) {
			if ( bStartupRebuild ) {
				LOG( ERROR,
					 "[Terrain3D][Startup][Material][PageTables] FAIL result=",
					 ( int ) ( page_table_result ),
					 " elapsed=",
					 Time::get_singleton()->get_ticks_usec() - page_tables_started_usec,
					 " us" );
			}
			if ( page_table_result == PAGE_TABLE_INVALID_TERRAIN_STATE && m_pTerrain && m_pTerrain->dataObject() ) {
				LOG( ERROR,
					 "Cannot build terrain page tables while data runtime storage is inactive: ",
					 m_pTerrain->dataObject()->getRuntimeState() );
			} else if ( page_table_result == PAGE_TABLE_INVALID_DIMENSIONS ) {
				LOG( ERROR, "Cannot build terrain page tables: invalid region-map dimensions" );
			} else if ( page_table_result == PAGE_TABLE_TEXTURE_ALLOCATION_FAILED ) {
				LOG( ERROR,
					 "Cannot build terrain page tables: generated RenderingDevice texture allocation/update "
					 "failed" );
			}
			return;
		}
		if ( bStartupRebuild ) {
			LOG( INFO,
				 "[Terrain3D][Startup][Material][PageTables] END elapsed=",
				 Time::get_singleton()->get_ticks_usec() - page_tables_started_usec,
				 " us" );
		}
	}
	IS_INIT( VOID );
	if ( m_pTerrain->getTessellationLevel() > 0 ) {
		const uint64_t snap_started_usec = Time::get_singleton()->get_ticks_usec();
		if ( bStartupRebuild ) {
			LOG( INFO, "[Terrain3D][Startup][Material][Snap] BEGIN" );
		}
		m_pTerrain->snap();
		if ( bStartupRebuild ) {
			LOG( INFO,
				 "[Terrain3D][Startup][Material][Snap] END elapsed=",
				 Time::get_singleton()->get_ticks_usec() - snap_started_usec,
				 " us" );
		}
	}
}

void Terrain3DMaterial::updateEditorOverlay(
	void ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || !m_pTerrain->dataObject()->isRuntimeReady() ) {
		return;
	}
	// Tool attachment and brush changes affect the overlay snapshot only.
	// Authoring flags still invalidate via their own material setters.
	m_runtimeState.m_nEditorOverlayGeneration =
		t3dNextGeneration( m_runtimeState.m_nEditorOverlayGeneration );
}

void Terrain3DMaterial::updateRegionPageTable(
	void ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	const t3dPageTableBuildResult_t page_table_result = prepareRegionPageTables();
	if ( page_table_result != PAGE_TABLE_READY ) {
		if ( page_table_result == PAGE_TABLE_INVALID_TERRAIN_STATE ) {
			LOG( DEBUG, "Skipping region page-table publication while terrain runtime storage is inactive" );
		} else if ( page_table_result == PAGE_TABLE_INVALID_DIMENSIONS ) {
			LOG( ERROR, "Cannot publish terrain page tables: invalid region-map dimensions" );
		} else {
			LOG( ERROR, "Cannot publish terrain page tables: texture allocation/update failed" );
		}
	}
}

void Terrain3DMaterial::setDisplacementScale(
	const real_t fDisplacementScale ) {
	SET_IF_DIFF( m_authoringState.m_fDisplacementScale, CLAMP( fDisplacementScale, 0.f, 2.f ) );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting displacement scale: ", fDisplacementScale );
	update();
}

void Terrain3DMaterial::setDisplacementSharpness(
	const real_t fDisplacementSharpness ) {
	SET_IF_DIFF( m_authoringState.m_fDisplacementSharpness, CLAMP( fDisplacementSharpness, 0.f, 1.f ) );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting displacement sharpness: ", fDisplacementSharpness );
	update();
	if ( m_pTerrain ) {
		m_pTerrain->snap();
	}
}

void Terrain3DMaterial::setWorldBackground(
	const t3dWorldBackground_t background ) {
	SET_IF_DIFF( m_authoringState.m_worldBackground, background );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable world background: ", background );
	update( FULL_REBUILD );
}

void Terrain3DMaterial::setShaderPerformancePreset(
	const t3dShaderPerformancePreset_t ePreset ) {
	if ( m_authoringState.m_shaderPerformancePreset == ePreset && ePreset != SHADER_PRESET_CUSTOM ) {
		return;
	}

	t3dShaderVariantState_t state = captureVariantState();
	applyPerformancePresetToState( state, ePreset );
	applyVariantState( state );
	applyPerformancePresetToTiers( ePreset );
	m_authoringState.m_shaderPerformancePreset = ePreset;
	LOG( INFO, "Setting shader performance preset: ", ePreset );
	// Rebuild native publication/schema state for all tiers.
	update( FULL_REBUILD );
	emit_changed();
}

void Terrain3DMaterial::updateTierUniforms(
	const t3dShaderTier_t tier ) {
	if ( !m_pTerrain || tier < SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT ) {
		return;
	}
#ifdef DEBUG_ENABLED
	++m_runtimeState.m_szTierUniformPublicationCount[tier];
#endif
}

void Terrain3DMaterial::tierQualityChanged(
	const t3dShaderTier_t tier,
	const bool			  bRecompile ) {
	( void ) bRecompile;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	refreshNativeMaterialState( false, false );
	updateTierUniforms( tier );
	emit_changed();
}

void Terrain3DMaterial::refreshGpuRegionChannelPolicy(
	void ) {
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return;
	}
	const uint64_t effective_budget_bytes = m_pTerrain->getEffectiveGpuRegionCacheBudgetBytes();
	m_pTerrain->reconfigureGpuRegionCache( effective_budget_bytes > 0u );
}

// UCMP-06: one authored source-quality record for the unified shader.
void Terrain3DMaterial::setCookedTextureFiltering( const t3dTextureFiltering_t filtering ) {
	setNearTextureFiltering( filtering );
}

t3dTextureFiltering_t Terrain3DMaterial::getCookedTextureFiltering( void ) const {
	return getNearTextureFiltering();
}

void Terrain3DMaterial::setCookedMaterialSamplingQuality( const t3dMaterialSamplingQuality_t quality ) {
	setNearMaterialSamplingQuality( quality );
}

t3dMaterialSamplingQuality_t Terrain3DMaterial::getCookedMaterialSamplingQuality( void ) const {
	return getNearMaterialSamplingQuality();
}

void Terrain3DMaterial::setCookedTerrainNormalQuality( const t3dTerrainNormalQuality_t quality ) {
	setNearTerrainNormalQuality( quality );
}

t3dTerrainNormalQuality_t Terrain3DMaterial::getCookedTerrainNormalQuality( void ) const {
	return getNearTerrainNormalQuality();
}

void Terrain3DMaterial::setCookedTerrainColorQuality( const t3dTerrainColorQuality_t quality ) {
	setNearTerrainColorQuality( quality );
}

t3dTerrainColorQuality_t Terrain3DMaterial::getCookedTerrainColorQuality( void ) const {
	return getNearTerrainColorQuality();
}

void Terrain3DMaterial::setCookedSurfaceResponseQuality( const t3dSurfaceResponseQuality_t quality ) {
	setNearSurfaceResponseQuality( quality );
}

t3dSurfaceResponseQuality_t Terrain3DMaterial::getCookedSurfaceResponseQuality( void ) const {
	return getNearSurfaceResponseQuality();
}

void Terrain3DMaterial::setCookedTextureSurfaceMode( const t3dTextureSurfaceMode_t mode ) {
	setNearTextureSurfaceMode( mode );
}

t3dTextureSurfaceMode_t Terrain3DMaterial::getCookedTextureSurfaceMode( void ) const {
	return getNearTextureSurfaceMode();
}

void Terrain3DMaterial::setCookedLightingModel( const t3dTierLightingModel_t model ) {
	setNearLightingModel( model );
}

t3dTierLightingModel_t Terrain3DMaterial::getCookedLightingModel( void ) const {
	return getNearLightingModel();
}

void Terrain3DMaterial::setCookedDetilingQuality( const t3dTierDetilingQuality_t quality ) {
	setNearDetilingQuality( quality );
}

t3dTierDetilingQuality_t Terrain3DMaterial::getCookedDetilingQuality( void ) const {
	return getNearDetilingQuality();
}

void Terrain3DMaterial::setCookedAuthoredColorContribution( const real_t contribution ) {
	setNearAuthoredColorContribution( contribution );
}

real_t Terrain3DMaterial::getCookedAuthoredColorContribution( void ) const {
	return getNearAuthoredColorContribution();
}

void Terrain3DMaterial::setCookedWetnessContribution( const real_t contribution ) {
	setNearWetnessContribution( contribution );
}

real_t Terrain3DMaterial::getCookedWetnessContribution( void ) const {
	return getNearWetnessContribution();
}

void Terrain3DMaterial::setCookedMaterialLodBias( const real_t bias ) {
	setNearMaterialLodBias( bias );
}

real_t Terrain3DMaterial::getCookedMaterialLodBias( void ) const {
	return getNearMaterialLodBias();
}

void Terrain3DMaterial::setNearTextureFiltering(
	const t3dTextureFiltering_t filtering ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_textureFiltering, filtering );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearMaterialSamplingQuality(
	const t3dMaterialSamplingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_materialSamplingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearTerrainNormalQuality(
	const t3dTerrainNormalQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_terrainNormalQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearTerrainColorQuality(
	const t3dTerrainColorQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_terrainColorQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearSurfaceResponseQuality(
	const t3dSurfaceResponseQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_surfaceResponseQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearTextureSurfaceMode(
	const t3dTextureSurfaceMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_textureSurfaceMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearLightingModel(
	const t3dTierLightingModel_t model ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_lightingModel, model );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearDetilingQuality(
	const t3dTierDetilingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_detilingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearAuthoredColorContribution(
	const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_fAuthoredColorContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_NEAR, false );
}

void Terrain3DMaterial::setNearWetnessContribution(
	const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_fWetnessContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_NEAR, false );
}

void Terrain3DMaterial::setNearMaterialLodBias(
	const real_t fBias ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_fMaterialLodBias, CLAMP( fBias, -2.0f, 4.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR, false );
}

void Terrain3DMaterial::setMiddleTextureFiltering(
	const t3dTextureFiltering_t filtering ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_textureFiltering, filtering );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleMaterialSamplingQuality(
	const t3dMaterialSamplingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_materialSamplingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleTerrainNormalQuality(
	const t3dTerrainNormalQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_terrainNormalQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleTerrainColorQuality(
	const t3dTerrainColorQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_terrainColorQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleSurfaceResponseQuality(
	const t3dSurfaceResponseQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_surfaceResponseQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleTextureSurfaceMode(
	const t3dTextureSurfaceMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_textureSurfaceMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleLightingModel(
	const t3dTierLightingModel_t model ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_lightingModel, model );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleDetilingQuality(
	const t3dTierDetilingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_detilingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleAuthoredColorContribution(
	const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_fAuthoredColorContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_MIDDLE, false );
}

void Terrain3DMaterial::setMiddleWetnessContribution(
	const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_fWetnessContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_MIDDLE, false );
}

void Terrain3DMaterial::setMiddleMaterialLodBias(
	const real_t fBias ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_fMaterialLodBias, CLAMP( fBias, -2.0f, 4.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE, false );
}

void Terrain3DMaterial::setFarTextureFiltering(
	const t3dTextureFiltering_t filtering ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_textureFiltering, filtering );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarMaterialSamplingQuality(
	const t3dMaterialSamplingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_materialSamplingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarTerrainNormalQuality(
	const t3dTerrainNormalQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_terrainNormalQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarTerrainColorQuality(
	const t3dTerrainColorQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_terrainColorQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarSurfaceResponseQuality(
	const t3dSurfaceResponseQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_surfaceResponseQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarTextureSurfaceMode(
	const t3dTextureSurfaceMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_textureSurfaceMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarLightingModel(
	const t3dTierLightingModel_t model ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_lightingModel, model );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarDetilingQuality(
	const t3dTierDetilingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_detilingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarAuthoredColorContribution(
	const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_fAuthoredColorContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR, false );
}

void Terrain3DMaterial::setFarWetnessContribution(
	const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_fWetnessContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR, false );
}

void Terrain3DMaterial::setFarMaterialLodBias(
	const real_t fBias ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_fMaterialLodBias, CLAMP( fBias, -2.0f, 4.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR, false );
}

// Hidden serialized aliases map old enum/value domains onto the canonical rows.
void Terrain3DMaterial::setTextureFiltering(
	const t3dTextureFiltering_t filtering ) {
	setNearTextureFiltering( filtering );
}

void Terrain3DMaterial::setMiddleTerrainSamplingMode(
	const t3dTerrainSamplingMode_t mode ) {
	setMiddleMaterialSamplingQuality( mode == TERRAIN_SAMPLING_SINGLE_SAMPLE ? MATERIAL_SAMPLING_FAST
																			 : MATERIAL_SAMPLING_FULL );
}

t3dTerrainSamplingMode_t Terrain3DMaterial::getMiddleTerrainSamplingMode(
	void ) const {
	return m_authoringState.m_middleQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST
			   ? TERRAIN_SAMPLING_SINGLE_SAMPLE
			   : TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED;
}

void Terrain3DMaterial::setMiddleTerrainNormalMode(
	const t3dTerrainNormalMode_t mode ) {
	setMiddleTerrainNormalQuality( mode == TERRAIN_NORMAL_GEOMETRY_DERIVATIVE ? TERRAIN_NORMAL_QUALITY_GEOMETRY
																			  : TERRAIN_NORMAL_QUALITY_DETAILED );
}

t3dTerrainNormalMode_t Terrain3DMaterial::getMiddleTerrainNormalMode(
	void ) const {
	return m_authoringState.m_middleQuality.m_terrainNormalQuality == TERRAIN_NORMAL_QUALITY_GEOMETRY
			   ? TERRAIN_NORMAL_GEOMETRY_DERIVATIVE
			   : TERRAIN_NORMAL_DERIVED_TEXTURE;
}

void Terrain3DMaterial::setMiddleColorSamplingMode(
	const t3dColorSamplingMode_t mode ) {
	setMiddleTerrainColorQuality( mode == COLOR_SAMPLING_EXACT_4_TAP ? TERRAIN_COLOR_QUALITY_EXACT
																	 : TERRAIN_COLOR_QUALITY_FAST );
}

t3dColorSamplingMode_t Terrain3DMaterial::getMiddleColorSamplingMode(
	void ) const {
	return m_authoringState.m_middleQuality.m_terrainColorQuality == TERRAIN_COLOR_QUALITY_EXACT
			   ? COLOR_SAMPLING_EXACT_4_TAP
			   : COLOR_SAMPLING_HARDWARE_FILTERED;
}

void Terrain3DMaterial::setMiddleSurfaceDataMode(
	const t3dSurfaceDataMode_t mode ) {
	setMiddleSurfaceResponseQuality( mode == SURFACE_DATA_ALBEDO_ONLY ? SURFACE_RESPONSE_SIMPLE
																	  : SURFACE_RESPONSE_DETAILED );
}

t3dSurfaceDataMode_t Terrain3DMaterial::getMiddleSurfaceDataMode(
	void ) const {
	return m_authoringState.m_middleQuality.m_surfaceResponseQuality == SURFACE_RESPONSE_DETAILED
			   ? SURFACE_DATA_FULL
			   : SURFACE_DATA_ALBEDO_ONLY;
}

void Terrain3DMaterial::setMiddleTerrainShadingMode(
	const t3dTerrainShadingMode_t mode ) {
	if ( mode == SHADING_UNSHADED ) {
		setMiddleLightingModel( TIER_LIGHTING_UNSHADED );
	} else if ( mode == SHADING_XBOX_360_BLINN_PHONG ) {
		setMiddleLightingModel( TIER_LIGHTING_BLINN_PHONG );
	} else if ( mode == SHADING_BURLEY_GGX || mode == SHADING_LAMBERT_GGX ) {
		setMiddleLightingModel( TIER_LIGHTING_PBR );
	} else {
		setMiddleLightingModel( TIER_LIGHTING_LAMBERT );
	}
}

t3dTerrainShadingMode_t Terrain3DMaterial::getMiddleTerrainShadingMode(
	void ) const {
	switch ( m_authoringState.m_middleQuality.m_lightingModel ) {
	case TIER_LIGHTING_UNSHADED:
		return SHADING_UNSHADED;
	case TIER_LIGHTING_BLINN_PHONG:
		return SHADING_XBOX_360_BLINN_PHONG;
	case TIER_LIGHTING_PBR:
		return SHADING_BURLEY_GGX;
	default:
		return SHADING_LAMBERT_DIFFUSE_ONLY;
	}
}

void Terrain3DMaterial::setMiddleDetilingMode(
	const t3dDetilingMode_t mode ) {
	setMiddleDetilingQuality( mode == DETILING_DISABLED ? TIER_DETILING_DISABLED : TIER_DETILING_ROTATION );
}

t3dDetilingMode_t Terrain3DMaterial::getMiddleDetilingMode(
	void ) const {
	return m_authoringState.m_middleQuality.m_detilingQuality == TIER_DETILING_DISABLED ? DETILING_DISABLED
																						: DETILING_DISCRETE_8;
}

void Terrain3DMaterial::setMiddleAutoShaderEnabled(
	const bool bEnabled ) {
	setAutoShaderEnabled( bEnabled );
}

void Terrain3DMaterial::setMiddleAuthoredColorEnabled(
	const bool bEnabled ) {
	setMiddleAuthoredColorContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setMiddleWetnessEnabled(
	const bool bEnabled ) {
	setMiddleWetnessContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setFarTerrainShadingMode(
	const t3dTerrainShadingMode_t mode ) {
	if ( mode == SHADING_UNSHADED ) {
		setFarLightingModel( TIER_LIGHTING_UNSHADED );
	} else if ( mode == SHADING_XBOX_360_BLINN_PHONG ) {
		setFarLightingModel( TIER_LIGHTING_BLINN_PHONG );
	} else if ( mode == SHADING_BURLEY_GGX || mode == SHADING_LAMBERT_GGX ) {
		setFarLightingModel( TIER_LIGHTING_PBR );
	} else {
		setFarLightingModel( TIER_LIGHTING_LAMBERT );
	}
}

t3dTerrainShadingMode_t Terrain3DMaterial::getFarTerrainShadingMode(
	void ) const {
	switch ( m_authoringState.m_farQuality.m_lightingModel ) {
	case TIER_LIGHTING_UNSHADED:
		return SHADING_UNSHADED;
	case TIER_LIGHTING_BLINN_PHONG:
		return SHADING_XBOX_360_BLINN_PHONG;
	case TIER_LIGHTING_PBR:
		return SHADING_BURLEY_GGX;
	default:
		return SHADING_LAMBERT_DIFFUSE_ONLY;
	}
}

void Terrain3DMaterial::setFarMaterialLod(
	const real_t fLod ) {
	setFarMaterialLodBias( CLAMP( fLod, 2.0f, 8.0f ) - 4.0f );
}

void Terrain3DMaterial::setFarAuthoredColorEnabled(
	const bool bEnabled ) {
	setFarAuthoredColorContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setFarWetnessEnabled(
	const bool bEnabled ) {
	setFarWetnessContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setTerrainSamplingMode(
	const t3dTerrainSamplingMode_t mode ) {
	setNearMaterialSamplingQuality( mode == TERRAIN_SAMPLING_SINGLE_SAMPLE ? MATERIAL_SAMPLING_FAST
																		   : MATERIAL_SAMPLING_FULL );
}

void Terrain3DMaterial::setTerrainNormalMode(
	const t3dTerrainNormalMode_t mode ) {
	setNearTerrainNormalQuality( mode == TERRAIN_NORMAL_GEOMETRY_DERIVATIVE ? TERRAIN_NORMAL_QUALITY_GEOMETRY
																			: TERRAIN_NORMAL_QUALITY_DETAILED );
}

void Terrain3DMaterial::setColorSamplingMode(
	const t3dColorSamplingMode_t mode ) {
	setNearTerrainColorQuality( mode == COLOR_SAMPLING_EXACT_4_TAP ? TERRAIN_COLOR_QUALITY_EXACT
																   : TERRAIN_COLOR_QUALITY_FAST );
}

void Terrain3DMaterial::setProjectionMode(
	const t3dProjectionMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_projectionMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	refreshNativeMaterialState( false, false );
	emit_changed();
}

void Terrain3DMaterial::setTessellationMaterialMode(
	const t3dTessellationMaterialMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_tessellationMaterialMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	refreshNativeMaterialState( false, false );
	emit_changed();
}

void Terrain3DMaterial::setSurfaceDataMode(
	const t3dSurfaceDataMode_t mode ) {
	// Match applyVariantState: legacy Auto retains surface response; only explicit albedo-only disables it.
	setNearSurfaceResponseQuality( mode == SURFACE_DATA_ALBEDO_ONLY ? SURFACE_RESPONSE_SIMPLE
																	: SURFACE_RESPONSE_DETAILED );
}

void Terrain3DMaterial::setTextureSurfaceMode(
	const t3dTextureSurfaceMode_t mode ) {
	setNearTextureSurfaceMode( mode );
}

void Terrain3DMaterial::setTerrainShadingMode(
	const t3dTerrainShadingMode_t mode ) {
	if ( mode == SHADING_UNSHADED ) {
		setNearLightingModel( TIER_LIGHTING_UNSHADED );
	} else if ( mode == SHADING_XBOX_360_BLINN_PHONG ) {
		setNearLightingModel( TIER_LIGHTING_BLINN_PHONG );
	} else if ( mode == SHADING_BURLEY_GGX || mode == SHADING_LAMBERT_GGX ) {
		setNearLightingModel( TIER_LIGHTING_PBR );
	} else {
		setNearLightingModel( TIER_LIGHTING_LAMBERT );
	}
}

void Terrain3DMaterial::setDetilingMode(
	const t3dDetilingMode_t mode ) {
	setNearDetilingQuality( mode == DETILING_DISABLED ? TIER_DETILING_DISABLED : TIER_DETILING_ROTATION );
}

void Terrain3DMaterial::setWorldNoiseShaderMode(
	const t3dWorldNoiseShaderMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_worldNoiseShaderMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Setting world-noise shader mode: ", mode );
	update( FULL_REBUILD );
	emit_changed();
}

String Terrain3DMaterial::getTierRangeContext(
	const t3dShaderTier_t tier ) const {
	if ( !m_pTerrain ) {
		return "Unavailable until attached to Terrain3D";
	}
	const real_t fNearEnd = m_pTerrain->getRuntimeNearRangeEndM();
	const real_t fMiddleEnd = m_pTerrain->getRuntimeMiddleRangeEndM();
	if ( tier == SHADER_TIER_NEAR ) {
		return vformat( "0 m - %.1f m", fNearEnd );
	}
	if ( tier == SHADER_TIER_MIDDLE ) {
		return vformat( "%.1f m - %.1f m", fNearEnd, fMiddleEnd );
	}
	return vformat( "%.1f m onward", fMiddleEnd );
}

String Terrain3DMaterial::getTierShadowContext(
	const t3dShaderTier_t tier ) const {
	if ( !m_pTerrain ) {
		return "Unavailable until attached to Terrain3D";
	}
	bool bEnabled = false;
	switch ( tier ) {
	case SHADER_TIER_NEAR:
		bEnabled = m_pTerrain->getNearShadowEnabled();
		break;
	case SHADER_TIER_MIDDLE:
		bEnabled = m_pTerrain->getMiddleShadowEnabled();
		break;
	case SHADER_TIER_FAR:
		bEnabled = m_pTerrain->getFarShadowEnabled();
		break;
	default:
		break;
	}
	if ( !bEnabled || m_pTerrain->getShadowLodCount() <= 0 ) {
		return "Disabled";
	}
	return vformat( "Enabled to %.1f m", m_pTerrain->getRuntimeShadowRangeEndM() );
}

Dictionary Terrain3DMaterial::getShaderTierDiagnostics(
	const int nTier ) const {
	Dictionary					  diagnostics;
	const int					  tier = CLAMP( nTier, ( int ) SHADER_TIER_NEAR, ( int ) SHADER_TIER_FAR );
	const t3dTierQualityConfig_t &quality =
		tier == SHADER_TIER_NEAR
			? m_authoringState.m_nearQuality
			: ( tier == SHADER_TIER_MIDDLE ? m_authoringState.m_middleQuality : m_authoringState.m_farQuality );
	diagnostics["tier"] = tier;
	diagnostics["backend"] = "rd_catalog";
	diagnostics["status"] = getTierShaderStatus( ( t3dShaderTier_t ) tier );
	diagnostics["compact_tier_active"] = false;
	diagnostics["texture_filtering"] = ( int ) quality.m_textureFiltering;
	diagnostics["material_sampling_quality"] = ( int ) quality.m_materialSamplingQuality;
	diagnostics["terrain_normal_quality"] = ( int ) quality.m_terrainNormalQuality;
	diagnostics["terrain_color_quality"] = ( int ) quality.m_terrainColorQuality;
	diagnostics["surface_response_quality"] = ( int ) quality.m_surfaceResponseQuality;
	diagnostics["shader_performance_preset"] = ( int ) m_authoringState.m_shaderPerformancePreset;
	diagnostics["material_publication_generation"] = ( int64_t ) m_runtimeState.m_nRdPublicationGeneration;
	diagnostics["editor_overlay_generation"] = ( int64_t ) m_runtimeState.m_nEditorOverlayGeneration;
	diagnostics["output_normal_map"] = m_authoringState.m_bOutputNormalMapEnabled;
	diagnostics["material_normal_maps_enabled"] =
		m_authoringState.m_bOutputNormalMapEnabled && quality.m_surfaceResponseQuality == SURFACE_RESPONSE_DETAILED;
	diagnostics["material_normal_gate"] =
		quality.m_surfaceResponseQuality == SURFACE_RESPONSE_SIMPLE
			? "Simple surface response disables material normals"
			: ( m_authoringState.m_bOutputNormalMapEnabled
					? "Detailed surface response; normal depth and residency still apply"
					: "Normal-map output disabled" );
	diagnostics["texture_surface_mode"] = ( int ) quality.m_textureSurfaceMode;
	diagnostics["lighting_model"] = ( int ) quality.m_lightingModel;
	diagnostics["detiling_quality"] = ( int ) quality.m_detilingQuality;
	diagnostics["authored_color_contribution"] = quality.m_fAuthoredColorContribution;
	diagnostics["wetness_contribution"] = quality.m_fWetnessContribution;
	diagnostics["material_lod_bias"] = quality.m_fMaterialLodBias;
	diagnostics["range_context"] = getTierRangeContext( ( t3dShaderTier_t ) tier );
	diagnostics["shadow_context"] = getTierShadowContext( ( t3dShaderTier_t ) tier );
	return diagnostics;
}

String Terrain3DMaterial::getTierShaderStatus(
	const t3dShaderTier_t ) const {
	return "RD catalog-managed";
}

void Terrain3DMaterial::setPackedPbrSingleFetch(
	const bool bEnabled ) {
	// Compatibility shim for scenes serialized by the experimental packed-PBR
	// branch. The current layout stores albedo and surface data in separate arrays,
	// so a true single-fetch path cannot be generated without changing the format.
	if ( bEnabled ) {
		LOG( WARN,
			 "packed_pbr_single_fetch is deprecated and has no valid path with the current texture layout; "
			 "ignoring it." );
	}
}

void Terrain3DMaterial::setAutoShaderEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bAutoShaderEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable auto shader: ", bEnabled );
	refreshNativeMaterialState( true, true );
	if ( m_pTerrain && m_pTerrain->dataObject() ) {
		m_pTerrain->dataObject()->invalidateGpuFarSurface();
	}
	emit_changed();
}

void Terrain3DMaterial::setAutoShaderEnabledTransient(
	const bool bEnabled ) {
	if ( m_authoringState.m_bAutoShaderEnabled == bEnabled ) {
		return;
	}
	m_authoringState.m_bAutoShaderEnabled = bEnabled;
	LOG( INFO, "Temporarily enable auto shader: ", bEnabled );
	refreshNativeMaterialState( true, true );
	if ( m_pTerrain && m_pTerrain->dataObject() ) {
		m_pTerrain->dataObject()->invalidateGpuFarSurface();
	}
}

void Terrain3DMaterial::setDualScalingEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDualScalingEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable dual scaling: ", bEnabled );
	refreshNativeMaterialState( true, true );
	emit_changed();
}

void Terrain3DMaterial::setMacroVariationEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bMacroVariationEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable macro variation: ", bEnabled );
	refreshNativeMaterialState( true, true );
	emit_changed();
}

void Terrain3DMaterial::setProjectionEnabled(
	const bool bEnabled ) {
	setProjectionMode( bEnabled ? PROJECTION_EXACT_PER_CORNER : PROJECTION_DISABLED );
}

void Terrain3DMaterial::setShaderParam(
	const StringName &strName,
	const Variant	 &rValue ) {
	LOG( DEBUG, "Setting shader parameter: ", strName );
	if ( strName.begins_with( "_" ) ) {
		setRuntimeMaterialParam( strName, rValue );
	} else {
		_set( strName, rValue );
	}
	if ( m_pTerrain && m_pTerrain->dataObject() &&
		 ( strName == StringName( "auto_slope" ) || strName == StringName( "auto_height_reduction" ) ||
		   strName == StringName( "auto_base_texture" ) || strName == StringName( "auto_overlay_texture" ) ) ) {
		m_pTerrain->dataObject()->invalidateGpuFarSurface();
	}
}

Variant Terrain3DMaterial::getShaderParam(
	const StringName &strName ) const {
	if ( strName.begins_with( "_" ) ) {
		return m_runtimeState.m_runtimeMaterialParams.get( strName, Variant() );
	}
	return m_authoringState.m_shaderParams.get( strName,
												m_authoringState.m_shaderParamDefaults.get( strName, Variant() ) );
}

void Terrain3DMaterial::setOutputAlbedoEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputAlbedoEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output albedo: ", bEnabled );
	refreshNativeMaterialState( false, false );
	emit_changed();
}

void Terrain3DMaterial::setOutputRoughnessEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputRoughnessEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output roughness: ", bEnabled );
	refreshNativeMaterialState( false, false );
	emit_changed();
}

void Terrain3DMaterial::setOutputSpecularEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputSpecularEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output specular: ", bEnabled );
	refreshNativeMaterialState( false, false );
	emit_changed();
}

void Terrain3DMaterial::setOutputNormalMapEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputNormalMapEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output normal map: ", bEnabled );
	refreshNativeMaterialState( false, false );
	emit_changed();
}

void Terrain3DMaterial::setOutputAmbientOcclusionEnabled(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputAmbientOcclusionEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output ambient occlusion: ", bEnabled );
	refreshNativeMaterialState( false, false );
	emit_changed();
}

void Terrain3DMaterial::setShowRegionGrid(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowRegionGrid, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_region_grid: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowInstancerGrid(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowInstancerGrid, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_instancer_grid: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowVertexGrid(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowVertexGrid, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_vertex_grid: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowContours(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowContours, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_contours: ", bEnabled );
	refreshNativeMaterialState( true, true );
}

void Terrain3DMaterial::setShowNavigation(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowNavigation, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_navigation: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowCheckered(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewCheckered, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable set_show_checkered: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowGrey(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewGrey, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_grey: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowHeightmap(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewHeightmap, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_heightmap: ", bEnabled );
	refreshNativeMaterialState( true, true );
}

void Terrain3DMaterial::setShowJaggedness(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewJaggedness, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_jaggedness: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowAutoshader(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewAutoshader, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_autoshader: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowControlTexture(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlTexture, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_texture: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowControlBlend(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlBlend, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_blend: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowControlAngle(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlAngle, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_angle: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowControlScale(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlScale, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_scale: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowColormap(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewColormap, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_colormap: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowRoughmap(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewRoughmap, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_roughmap: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowTextureAlbedo(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexAlbedo, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_albedo: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowTextureHeight(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexHeight, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_height: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowTextureNormal(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexNormal, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_normal: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowTextureRough(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexRough, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_rough: ", bEnabled );
	refreshNativeMaterialState( false, false );
}
void Terrain3DMaterial::setShowDisplacementBuffer(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewDisplacementBuffer, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_displacement_buffer: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

void Terrain3DMaterial::setShowTextureAo(
	const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexAo, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_ao: ", bEnabled );
	refreshNativeMaterialState( false, false );
}

Error Terrain3DMaterial::save(
	const String &rPath ) {
	return t3dSaveExternalResource( this, rPath, m_authoringState.m_bPersistenceDirty, [this]() {
		// Remove saved parameters that are not active in the native material schema.
		// Saving never depends on shader compilation or reflection.
		const Array keys = m_authoringState.m_shaderParams.keys();
		for ( size_t iKey = 0u; iKey < ( size_t ) ( ( keys ).size() ); iKey++ ) {
			const StringName name = ( keys )[iKey];
			if ( !m_runtimeState.m_activeParams.has( name ) ) {
				LOG( DEBUG, "'", name, "' not found in shader parameters. Removing from dictionary." );
				m_authoringState.m_shaderParams.erase( name );
				m_authoringState.m_bPersistenceDirty = true;
			}
		}
	} );
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DMaterial::refreshMaterialPropertyCache(
	void ) {
	m_authoringState.m_shaderPropertyCache.clear();
	m_authoringState.m_shaderParamDefaults.clear();
	m_runtimeState.m_activeParams.clear();

	// Defaults are schema authority, not active-shader authority. Keep every public
	// parameter addressable even when its inspector group is currently hidden.
	size_t								  nSchemaEntryCount = 0u;
	const t3dMaterialSchemaEntry_t *const pSchemaEntries = t3dMaterialSchemaEntries( nSchemaEntryCount );
	for ( size_t iEntry = 0u; iEntry < nSchemaEntryCount; ++iEntry ) {
		const t3dMaterialSchemaEntry_t &entry = pSchemaEntries[iEntry];
		if ( !entry.m_bGroup ) {
			m_authoringState.m_shaderParamDefaults[StringName( entry.m_pszName )] =
				t3dMaterialSchemaDefaultToVariant( entry );
		}
	}

	const auto is_visible = [this]( const t3dMaterialSchemaVisibility_t visibility ) {
		switch ( visibility ) {
		case T3D_MATERIAL_SCHEMA_WORLD_FLAT:
			return m_authoringState.m_worldBackground == FLAT;
		case T3D_MATERIAL_SCHEMA_AUTO_SHADER:
			return m_authoringState.m_bAutoShaderEnabled;
		case T3D_MATERIAL_SCHEMA_DUAL_SCALING:
			return m_authoringState.m_bDualScalingEnabled;
		case T3D_MATERIAL_SCHEMA_MACRO_VARIATION:
			return m_authoringState.m_bMacroVariationEnabled;
		case T3D_MATERIAL_SCHEMA_WORLD_NOISE:
			return m_authoringState.m_worldBackground == NOISE;
		case T3D_MATERIAL_SCHEMA_DEBUG_HEIGHTMAP:
			return m_authoringState.m_bDebugViewHeightmap;
		case T3D_MATERIAL_SCHEMA_CONTOURS:
			return m_authoringState.m_bShowContours;
		case T3D_MATERIAL_SCHEMA_ALWAYS:
		default:
			return true;
		}
	};

	for ( size_t iEntry = 0u; iEntry < nSchemaEntryCount; ++iEntry ) {
		const t3dMaterialSchemaEntry_t &entry = pSchemaEntries[iEntry];
		if ( !is_visible( entry.m_visibility ) ) {
			continue;
		}

		PropertyInfo property;
		property.name = StringName( entry.m_pszName );
		property.class_name = StringName();
		property.type = entry.m_type;
		property.hint = entry.m_hint;
		property.hint_string = String( entry.m_pszHintString );
		property.usage = entry.m_bGroup ? PROPERTY_USAGE_GROUP : PROPERTY_USAGE_EDITOR;
		m_authoringState.m_shaderPropertyCache.push_back( property );

		if ( entry.m_bGroup ) {
			continue;
		}
		const StringName name( entry.m_pszName );
		m_runtimeState.m_activeParams.push_back( name );
		if ( !m_authoringState.m_shaderParams.has( name ) ) {
			m_authoringState.m_shaderParams[name] = t3dMaterialSchemaDefaultToVariant( entry );
		}
	}
}

// Add the native material parameter schema cached by refreshMaterialPropertyCache().
void Terrain3DMaterial::_get_property_list(
	List< PropertyInfo > *const pList ) const {
	Resource::_get_property_list( pList );
	for ( size_t iProperty = 0u; iProperty < ( size_t ) ( ( m_authoringState.m_shaderPropertyCache ).size() );
		  iProperty++ ) {
		const PropertyInfo &property = ( m_authoringState.m_shaderPropertyCache )[iProperty];
		pList->push_back( property );
	}
}

// Only the inspector projection changes; Godot still serializes every historical
// tier property so old .tres files survive mode switches without data loss.
void Terrain3DMaterial::_validate_property( PropertyInfo &rProperty ) const {
    const bool bUnified = Terrain3DRdShaderLibrary::isUnifiedCookedPreviewEnabledCold();
    const String name = String( rProperty.name );
    const bool bLegacyTier = name.begins_with( "near_" ) ||
        name.begins_with( "middle_" ) || name.begins_with( "far_" );
    if ( ( bUnified && bLegacyTier ) ||
         ( !bUnified && name.begins_with( "cooked_" ) ) ) {
        rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
    }
}

// Flag uniforms with non-default values
// This is called 10x more than the others, so be efficient
bool Terrain3DMaterial::_property_can_revert(
	const StringName &strName ) const {
	if ( !m_runtimeState.m_activeParams.has( strName ) ) {
		return Resource::_property_can_revert( strName );
	}
	const Variant default_value = m_authoringState.m_shaderParamDefaults.get( strName, Variant() );
	const Variant current_value = m_authoringState.m_shaderParams.get( strName, default_value );
	return default_value != current_value;
}

// Provide uniform default values in r_property
bool Terrain3DMaterial::_property_get_revert(
	const StringName &strName,
	Variant			 &rProperty ) const {
	if ( !m_runtimeState.m_activeParams.has( strName ) ) {
		return Resource::_property_get_revert( strName, rProperty );
	}
	rProperty = m_authoringState.m_shaderParamDefaults.get( strName, Variant() );
	return true;
}

bool Terrain3DMaterial::_set(
	const StringName &strName,
	const Variant	 &rProperty ) {
	if ( !m_runtimeState.m_activeParams.has( strName ) ) {
		return Resource::_set( strName, rProperty );
	}
	m_authoringState.m_bPersistenceDirty = true;
	if ( rProperty.get_type() == Variant::NIL ) {
		setRuntimeMaterialParam( strName, Variant() );
		m_authoringState.m_shaderParams.erase( strName );
		return true;
	}

	// Preserve Resource objects in authoring state; the RD coordinator resolves
	// texture RIDs when publishing an immutable render snapshot.
	if ( rProperty.get_type() == Variant::OBJECT ) {
		const Ref< Texture > pTex = rProperty;
		if ( pTex.is_valid() ) {
			m_authoringState.m_shaderParams[strName] = pTex;
			setRuntimeMaterialParam( strName, pTex );
		} else {
			m_authoringState.m_shaderParams.erase( strName );
			setRuntimeMaterialParam( strName, Variant() );
		}
	} else {
		m_authoringState.m_shaderParams[strName] = rProperty;
		setRuntimeMaterialParam( strName, rProperty );
		if ( strName == StringName( "noise1_angle" ) ) {
			const real_t  fAngle = rProperty;
			const Vector2 rotation( Math::cos( fAngle ), Math::sin( fAngle ) );
			setRuntimeMaterialParam( "_noise1_rotation", rotation );
		}
	}
	return true;
}

// This is called continuously while the material is open in the inspector.
// Serve it entirely from the native schema-backed authoring state.
bool Terrain3DMaterial::_get(
	const StringName &strName,
	Variant			 &rProperty ) const {
	if ( !m_runtimeState.m_activeParams.has( strName ) ) {
		return Resource::_get( strName, rProperty );
	}
	++m_runtimeState.m_nShaderInspectorRuntimeReadCount;
	rProperty = m_authoringState.m_shaderParams.get( strName,
													 m_authoringState.m_shaderParamDefaults.get( strName, Variant() ) );
	return true;
}

void Terrain3DMaterial::_bind_methods(
	void ) {
	BIND_ENUM_CONSTANT( NONE );
	BIND_ENUM_CONSTANT( FLAT );
	BIND_ENUM_CONSTANT( NOISE );
	BIND_ENUM_CONSTANT( LINEAR_ANISOTROPIC );
	BIND_ENUM_CONSTANT( LINEAR );
	BIND_ENUM_CONSTANT( NEAREST_ANISOTROPIC );
	BIND_ENUM_CONSTANT( NEAREST );
	BIND_ENUM_CONSTANT( SHADER_PRESET_CUSTOM );
	BIND_ENUM_CONSTANT( SHADER_PRESET_CINEMATIC );
	BIND_ENUM_CONSTANT( SHADER_PRESET_HIGH );
	BIND_ENUM_CONSTANT( SHADER_PRESET_RUNTIME_360 );
	BIND_ENUM_CONSTANT( SHADER_PRESET_LOW );
	BIND_ENUM_CONSTANT( SHADER_PRESET_MINIMUM );
	BIND_ENUM_CONSTANT( SHADER_PRESET_XBOX_360_AUTHENTIC );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ADAPTIVE );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ALWAYS_SMOOTH );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_SINGLE_SAMPLE );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_DISTANCE_ADAPTIVE );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ADAPTIVE_2_CORNER );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_ADAPTIVE );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_HEIGHT_8_TAP );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_GEOMETRY_DERIVATIVE );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_DERIVED_TEXTURE );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_HEIGHT_3_TAP );
	BIND_ENUM_CONSTANT( COLOR_SAMPLING_AUTO );
	BIND_ENUM_CONSTANT( COLOR_SAMPLING_HARDWARE_FILTERED );
	BIND_ENUM_CONSTANT( COLOR_SAMPLING_EXACT_4_TAP );
	BIND_ENUM_CONSTANT( PROJECTION_DISABLED );
	BIND_ENUM_CONSTANT( PROJECTION_SHARED_FRAGMENT );
	BIND_ENUM_CONSTANT( PROJECTION_EXACT_PER_CORNER );
	BIND_ENUM_CONSTANT( TESSELLATION_GEOMETRY_ONLY );
	BIND_ENUM_CONSTANT( TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT );
	BIND_ENUM_CONSTANT( SURFACE_DATA_AUTO );
	BIND_ENUM_CONSTANT( SURFACE_DATA_FULL );
	BIND_ENUM_CONSTANT( SURFACE_DATA_ALBEDO_ONLY );
	BIND_ENUM_CONSTANT( TEXTURE_SURFACE_TEXTURED );
	BIND_ENUM_CONSTANT( TEXTURE_SURFACE_SILHOUETTE );
	BIND_ENUM_CONSTANT( SHADING_BURLEY_GGX );
	BIND_ENUM_CONSTANT( SHADING_LAMBERT_GGX );
	BIND_ENUM_CONSTANT( SHADING_LAMBERT_DIFFUSE_ONLY );
	BIND_ENUM_CONSTANT( SHADING_UNSHADED );
	BIND_ENUM_CONSTANT( SHADING_LAMBERT_VERTEX );
	BIND_ENUM_CONSTANT( SHADING_XBOX_360_BLINN_PHONG );
	BIND_ENUM_CONSTANT( MATERIAL_SAMPLING_FAST );
	BIND_ENUM_CONSTANT( MATERIAL_SAMPLING_FULL );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_QUALITY_GEOMETRY );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_QUALITY_DETAILED );
	BIND_ENUM_CONSTANT( TERRAIN_COLOR_QUALITY_FAST );
	BIND_ENUM_CONSTANT( TERRAIN_COLOR_QUALITY_EXACT );
	BIND_ENUM_CONSTANT( SURFACE_RESPONSE_SIMPLE );
	BIND_ENUM_CONSTANT( SURFACE_RESPONSE_DETAILED );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_UNSHADED );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_LAMBERT );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_BLINN_PHONG );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_PBR );
	BIND_ENUM_CONSTANT( TIER_DETILING_DISABLED );
	BIND_ENUM_CONSTANT( TIER_DETILING_ROTATION );
	BIND_ENUM_CONSTANT( DETILING_CONTINUOUS );
	BIND_ENUM_CONSTANT( DETILING_DISCRETE_8 );
	BIND_ENUM_CONSTANT( DETILING_DISABLED );
	BIND_ENUM_CONSTANT( DETILING_TRANSLATION_ONLY );
	BIND_ENUM_CONSTANT( DETILING_DISCRETE_4 );
	BIND_ENUM_CONSTANT( DETILING_DISCRETE_16 );
	BIND_ENUM_CONSTANT( WORLD_NOISE_DYNAMIC );
	BIND_ENUM_CONSTANT( WORLD_NOISE_CACHED_VERTEX_NORMALS );
	BIND_ENUM_CONSTANT( WORLD_NOISE_CACHED_FRAGMENT_NORMALS );
	BIND_ENUM_CONSTANT( WORLD_NOISE_PROCEDURAL_VERTEX_NORMALS );
	BIND_ENUM_CONSTANT( WORLD_NOISE_PROCEDURAL_FRAGMENT_NORMALS );
	BIND_ENUM_CONSTANT( UNIFORMS_ONLY );
	BIND_ENUM_CONSTANT( TEXTURE_ARRAYS );
	BIND_ENUM_CONSTANT( REGION_ARRAYS );
	BIND_ENUM_CONSTANT( UPDATE_ARRAYS );
	BIND_ENUM_CONSTANT( FULL_REBUILD );

	// Private
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShaderParameters,
		getShaderParameters,
		"dict",
		PropertyInfo( Variant::DICTIONARY, "_shader_parameters", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"_set_shader_parameters",
		"_get_shader_parameters" );

	// Public
	ClassDB::bind_method( D_METHOD( "update", "flags" ), &Terrain3DMaterial::update, DEFVAL( UNIFORMS_ONLY ) );
	ClassDB::bind_method( D_METHOD( "update_editor_overlay" ), &Terrain3DMaterial::updateEditorOverlay );
	ClassDB::bind_method( D_METHOD( "is_render_shader_ready" ), &Terrain3DMaterial::isRenderShaderReady );
	ClassDB::bind_method( D_METHOD( "is_region_page_table_valid" ), &Terrain3DMaterial::isRegionPageTableValid );

	ClassDB::bind_method( D_METHOD( "get_near_range_context" ), &Terrain3DMaterial::getNearRangeContext );
	ClassDB::bind_method( D_METHOD( "get_middle_range_context" ), &Terrain3DMaterial::getMiddleRangeContext );
	ClassDB::bind_method( D_METHOD( "get_far_range_context" ), &Terrain3DMaterial::getFarRangeContext );
	ClassDB::bind_method( D_METHOD( "get_near_shadow_context" ), &Terrain3DMaterial::getNearShadowContext );
	ClassDB::bind_method( D_METHOD( "get_middle_shadow_context" ), &Terrain3DMaterial::getMiddleShadowContext );
	ClassDB::bind_method( D_METHOD( "get_far_shadow_context" ), &Terrain3DMaterial::getFarShadowContext );
	ClassDB::bind_method( D_METHOD( "get_near_shader_status" ), &Terrain3DMaterial::getNearShaderStatus );
	ClassDB::bind_method( D_METHOD( "get_middle_shader_status" ), &Terrain3DMaterial::getMiddleShaderStatus );
	ClassDB::bind_method( D_METHOD( "get_far_shader_status" ), &Terrain3DMaterial::getFarShaderStatus );

	ClassDB::bind_method( D_METHOD( "get_shader_tier_diagnostics", "tier" ),
						  &Terrain3DMaterial::getShaderTierDiagnostics );
	ClassDB::bind_method( D_METHOD( "set_auto_shader_enabled", "enabled" ), &Terrain3DMaterial::setAutoShaderEnabled );
	ClassDB::bind_method( D_METHOD( "set_auto_shader_enabled_transient", "enabled" ),
						  &Terrain3DMaterial::setAutoShaderEnabledTransient );
	ClassDB::bind_method( D_METHOD( "get_auto_shader_enabled" ), &Terrain3DMaterial::getAutoShaderEnabled );

	ClassDB::bind_method( D_METHOD( "set_shader_param", "name", "value" ), &Terrain3DMaterial::setShaderParam );
	ClassDB::bind_method( D_METHOD( "get_shader_param", "name" ), &Terrain3DMaterial::getShaderParam );

	// PBR output

	// Overlays

	// Debug Views

	// PBR Views

	ClassDB::bind_method( D_METHOD( "save", "path" ), &Terrain3DMaterial::save, DEFVAL( "" ) );

	// Inspector groups are intentionally distinct from shader uniform-group names.

	ADD_GROUP( "Material Setup", "" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setShaderPerformancePreset,
					   getShaderPerformancePreset,
					   "preset",
					   PropertyInfo( Variant::INT,
									 "shader_performance_preset",
									 PROPERTY_HINT_ENUM,
									 "Custom,Cinematic,High,Xbox 360 Styled,Low,Minimum,Xbox 360 Authentic" ),
					   "set_shader_performance_preset",
					   "get_shader_performance_preset" );

	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setWorldBackground,
					   getWorldBackground,
					   "background",
					   PropertyInfo( Variant::INT, "world_background", PROPERTY_HINT_ENUM, "None,Flat,Noise" ),
					   "set_world_background",
					   "get_world_background" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setWorldNoiseShaderMode,
					   getWorldNoiseShaderMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "world_noise_shader_mode",
									 PROPERTY_HINT_ENUM,
									 "Dynamic,Cached + Vertex Normals,Cached + Fragment Normals,Procedural + Vertex "
									 "Normals,Procedural + Fragment Normals" ),
					   "set_world_noise_shader_mode",
					   "get_world_noise_shader_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "auto_shader_enabled" ),
				  "set_auto_shader_enabled",
				  "get_auto_shader_enabled" );

	// UCMP-06: these aliases edit the same persisted near/source settings.
	ClassDB::bind_method( D_METHOD( "set_cooked_texture_filtering", "filtering" ), &Terrain3DMaterial::setCookedTextureFiltering );
	ClassDB::bind_method( D_METHOD( "get_cooked_texture_filtering" ), &Terrain3DMaterial::getCookedTextureFiltering );
	ClassDB::bind_method( D_METHOD( "set_cooked_material_sampling_quality", "quality" ), &Terrain3DMaterial::setCookedMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "get_cooked_material_sampling_quality" ), &Terrain3DMaterial::getCookedMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "set_cooked_terrain_normal_quality", "quality" ), &Terrain3DMaterial::setCookedTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "get_cooked_terrain_normal_quality" ), &Terrain3DMaterial::getCookedTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "set_cooked_terrain_color_quality", "quality" ), &Terrain3DMaterial::setCookedTerrainColorQuality );
	ClassDB::bind_method( D_METHOD( "get_cooked_terrain_color_quality" ), &Terrain3DMaterial::getCookedTerrainColorQuality );
	ClassDB::bind_method( D_METHOD( "set_cooked_surface_response_quality", "quality" ), &Terrain3DMaterial::setCookedSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "get_cooked_surface_response_quality" ), &Terrain3DMaterial::getCookedSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "set_cooked_texture_surface_mode", "mode" ), &Terrain3DMaterial::setCookedTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "get_cooked_texture_surface_mode" ), &Terrain3DMaterial::getCookedTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "set_cooked_lighting_model", "model" ), &Terrain3DMaterial::setCookedLightingModel );
	ClassDB::bind_method( D_METHOD( "get_cooked_lighting_model" ), &Terrain3DMaterial::getCookedLightingModel );
	ClassDB::bind_method( D_METHOD( "set_cooked_detiling_quality", "quality" ), &Terrain3DMaterial::setCookedDetilingQuality );
	ClassDB::bind_method( D_METHOD( "get_cooked_detiling_quality" ), &Terrain3DMaterial::getCookedDetilingQuality );
	ClassDB::bind_method( D_METHOD( "set_cooked_authored_color_contribution", "contribution" ), &Terrain3DMaterial::setCookedAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "get_cooked_authored_color_contribution" ), &Terrain3DMaterial::getCookedAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "set_cooked_wetness_contribution", "contribution" ), &Terrain3DMaterial::setCookedWetnessContribution );
	ClassDB::bind_method( D_METHOD( "get_cooked_wetness_contribution" ), &Terrain3DMaterial::getCookedWetnessContribution );
	ClassDB::bind_method( D_METHOD( "set_cooked_material_lod_bias", "bias" ), &Terrain3DMaterial::setCookedMaterialLodBias );
	ClassDB::bind_method( D_METHOD( "get_cooked_material_lod_bias" ), &Terrain3DMaterial::getCookedMaterialLodBias );
	ADD_GROUP( "Cooked Material Quality", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_texture_filtering", PROPERTY_HINT_ENUM, "Linear Anisotropic,Linear,Nearest Anisotropic,Nearest", PROPERTY_USAGE_EDITOR ),
		"set_cooked_texture_filtering", "get_cooked_texture_filtering" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_material_sampling_quality", PROPERTY_HINT_ENUM, "Fast,Full", PROPERTY_USAGE_EDITOR ),
		"set_cooked_material_sampling_quality", "get_cooked_material_sampling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_terrain_normal_quality", PROPERTY_HINT_ENUM, "Geometry,Detailed", PROPERTY_USAGE_EDITOR ),
		"set_cooked_terrain_normal_quality", "get_cooked_terrain_normal_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_terrain_color_quality", PROPERTY_HINT_ENUM, "Fast,Exact", PROPERTY_USAGE_EDITOR ),
		"set_cooked_terrain_color_quality", "get_cooked_terrain_color_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_surface_response_quality", PROPERTY_HINT_ENUM, "Simple,Detailed", PROPERTY_USAGE_EDITOR ),
		"set_cooked_surface_response_quality", "get_cooked_surface_response_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_texture_surface_mode", PROPERTY_HINT_ENUM, "Textured,Silhouette / No Textures", PROPERTY_USAGE_EDITOR ),
		"set_cooked_texture_surface_mode", "get_cooked_texture_surface_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_lighting_model", PROPERTY_HINT_ENUM, "Unshaded,Lambert,Blinn-Phong,PBR", PROPERTY_USAGE_EDITOR ),
		"set_cooked_lighting_model", "get_cooked_lighting_model" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cooked_detiling_quality", PROPERTY_HINT_ENUM, "Disabled,Rotation", PROPERTY_USAGE_EDITOR ),
		"set_cooked_detiling_quality", "get_cooked_detiling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cooked_authored_color_contribution", PROPERTY_HINT_RANGE, "0,1,0.01", PROPERTY_USAGE_EDITOR ),
		"set_cooked_authored_color_contribution", "get_cooked_authored_color_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cooked_wetness_contribution", PROPERTY_HINT_RANGE, "0,1,0.01", PROPERTY_USAGE_EDITOR ),
		"set_cooked_wetness_contribution", "get_cooked_wetness_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cooked_material_lod_bias", PROPERTY_HINT_RANGE, "-2,4,0.25", PROPERTY_USAGE_EDITOR ),
		"set_cooked_material_lod_bias", "get_cooked_material_lod_bias" );

	ADD_GROUP( "Near Terrain Detail", "" );
	ADD_SUBGROUP( "Sampling & Surface", "" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setNearTextureFiltering,
					   getNearTextureFiltering,
					   "filtering",
					   PropertyInfo( Variant::INT,
									 "near_texture_filtering",
									 PROPERTY_HINT_ENUM,
									 "Linear Anisotropic,Linear,Nearest Anisotropic,Nearest" ),
					   "set_near_texture_filtering",
					   "get_near_texture_filtering" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setNearMaterialSamplingQuality,
					   getNearMaterialSamplingQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "near_material_sampling_quality", PROPERTY_HINT_ENUM, "Fast,Full" ),
					   "set_near_material_sampling_quality",
					   "get_near_material_sampling_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setNearTerrainNormalQuality,
		getNearTerrainNormalQuality,
		"quality",
		PropertyInfo( Variant::INT, "near_terrain_normal_quality", PROPERTY_HINT_ENUM, "Geometry,Detailed" ),
		"set_near_terrain_normal_quality",
		"get_near_terrain_normal_quality" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setNearTerrainColorQuality,
					   getNearTerrainColorQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "near_terrain_color_quality", PROPERTY_HINT_ENUM, "Fast,Exact" ),
					   "set_near_terrain_color_quality",
					   "get_near_terrain_color_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setNearSurfaceResponseQuality,
		getNearSurfaceResponseQuality,
		"quality",
		PropertyInfo( Variant::INT, "near_surface_response_quality", PROPERTY_HINT_ENUM, "Simple,Detailed" ),
		"set_near_surface_response_quality",
		"get_near_surface_response_quality" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setNearTextureSurfaceMode,
					   getNearTextureSurfaceMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "near_texture_surface_mode",
									 PROPERTY_HINT_ENUM,
									 "Textured,Silhouette / No Textures" ),
					   "set_near_texture_surface_mode",
					   "get_near_texture_surface_mode" );
	ADD_SUBGROUP( "Lighting & Texture Blending", "" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setNearLightingModel,
		getNearLightingModel,
		"model",
		PropertyInfo( Variant::INT, "near_lighting_model", PROPERTY_HINT_ENUM, "Unshaded,Lambert,Blinn-Phong,PBR" ),
		"set_near_lighting_model",
		"get_near_lighting_model" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setNearDetilingQuality,
					   getNearDetilingQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "near_detiling_quality", PROPERTY_HINT_ENUM, "Disabled,Rotation" ),
					   "set_near_detiling_quality",
					   "get_near_detiling_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setNearAuthoredColorContribution,
		getNearAuthoredColorContribution,
		"contribution",
		PropertyInfo( Variant::FLOAT, "near_authored_color_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
		"set_near_authored_color_contribution",
		"get_near_authored_color_contribution" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setNearWetnessContribution,
					   getNearWetnessContribution,
					   "contribution",
					   PropertyInfo( Variant::FLOAT, "near_wetness_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
					   "set_near_wetness_contribution",
					   "get_near_wetness_contribution" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setNearMaterialLodBias,
					   getNearMaterialLodBias,
					   "bias",
					   PropertyInfo( Variant::FLOAT, "near_material_lod_bias", PROPERTY_HINT_RANGE, "-2,4,0.25" ),
					   "set_near_material_lod_bias",
					   "get_near_material_lod_bias" );

	ADD_GROUP( "Middle Terrain Detail", "" );
	ADD_SUBGROUP( "Sampling & Surface", "" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setMiddleTextureFiltering,
					   getMiddleTextureFiltering,
					   "filtering",
					   PropertyInfo( Variant::INT,
									 "middle_texture_filtering",
									 PROPERTY_HINT_ENUM,
									 "Linear Anisotropic,Linear,Nearest Anisotropic,Nearest" ),
					   "set_middle_texture_filtering",
					   "get_middle_texture_filtering" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleMaterialSamplingQuality,
		getMiddleMaterialSamplingQuality,
		"quality",
		PropertyInfo( Variant::INT, "middle_material_sampling_quality", PROPERTY_HINT_ENUM, "Fast,Full" ),
		"set_middle_material_sampling_quality",
		"get_middle_material_sampling_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleSurfaceSource,
		getMiddleSurfaceSource,
		"source",
		PropertyInfo( Variant::INT, "middle_surface_source", PROPERTY_HINT_ENUM, "Baked Composite,Runtime Material" ),
		"set_middle_surface_source",
		"get_middle_surface_source" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleTerrainNormalQuality,
		getMiddleTerrainNormalQuality,
		"quality",
		PropertyInfo( Variant::INT, "middle_terrain_normal_quality", PROPERTY_HINT_ENUM, "Geometry,Detailed" ),
		"set_middle_terrain_normal_quality",
		"get_middle_terrain_normal_quality" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setMiddleTerrainColorQuality,
					   getMiddleTerrainColorQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "middle_terrain_color_quality", PROPERTY_HINT_ENUM, "Fast,Exact" ),
					   "set_middle_terrain_color_quality",
					   "get_middle_terrain_color_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleSurfaceResponseQuality,
		getMiddleSurfaceResponseQuality,
		"quality",
		PropertyInfo( Variant::INT, "middle_surface_response_quality", PROPERTY_HINT_ENUM, "Simple,Detailed" ),
		"set_middle_surface_response_quality",
		"get_middle_surface_response_quality" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setMiddleTextureSurfaceMode,
					   getMiddleTextureSurfaceMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "middle_texture_surface_mode",
									 PROPERTY_HINT_ENUM,
									 "Textured,Silhouette / No Textures" ),
					   "set_middle_texture_surface_mode",
					   "get_middle_texture_surface_mode" );
	ADD_SUBGROUP( "Lighting & Texture Blending", "" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleLightingModel,
		getMiddleLightingModel,
		"model",
		PropertyInfo( Variant::INT, "middle_lighting_model", PROPERTY_HINT_ENUM, "Unshaded,Lambert,Blinn-Phong,PBR" ),
		"set_middle_lighting_model",
		"get_middle_lighting_model" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setMiddleDetilingQuality,
					   getMiddleDetilingQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "middle_detiling_quality", PROPERTY_HINT_ENUM, "Disabled,Rotation" ),
					   "set_middle_detiling_quality",
					   "get_middle_detiling_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleAuthoredColorContribution,
		getMiddleAuthoredColorContribution,
		"contribution",
		PropertyInfo( Variant::FLOAT, "middle_authored_color_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
		"set_middle_authored_color_contribution",
		"get_middle_authored_color_contribution" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setMiddleWetnessContribution,
					   getMiddleWetnessContribution,
					   "contribution",
					   PropertyInfo( Variant::FLOAT, "middle_wetness_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
					   "set_middle_wetness_contribution",
					   "get_middle_wetness_contribution" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setMiddleMaterialLodBias,
					   getMiddleMaterialLodBias,
					   "bias",
					   PropertyInfo( Variant::FLOAT, "middle_material_lod_bias", PROPERTY_HINT_RANGE, "-2,4,0.25" ),
					   "set_middle_material_lod_bias",
					   "get_middle_material_lod_bias" );

	ADD_GROUP( "Far Terrain Detail", "" );
	ADD_SUBGROUP( "Sampling & Surface", "" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setFarTextureFiltering,
					   getFarTextureFiltering,
					   "filtering",
					   PropertyInfo( Variant::INT,
									 "far_texture_filtering",
									 PROPERTY_HINT_ENUM,
									 "Linear Anisotropic,Linear,Nearest Anisotropic,Nearest" ),
					   "set_far_texture_filtering",
					   "get_far_texture_filtering" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setFarMaterialSamplingQuality,
					   getFarMaterialSamplingQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "far_material_sampling_quality", PROPERTY_HINT_ENUM, "Fast,Full" ),
					   "set_far_material_sampling_quality",
					   "get_far_material_sampling_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarTerrainNormalQuality,
		getFarTerrainNormalQuality,
		"quality",
		PropertyInfo( Variant::INT, "far_terrain_normal_quality", PROPERTY_HINT_ENUM, "Geometry,Detailed" ),
		"set_far_terrain_normal_quality",
		"get_far_terrain_normal_quality" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setFarTerrainColorQuality,
					   getFarTerrainColorQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "far_terrain_color_quality", PROPERTY_HINT_ENUM, "Fast,Exact" ),
					   "set_far_terrain_color_quality",
					   "get_far_terrain_color_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarSurfaceResponseQuality,
		getFarSurfaceResponseQuality,
		"quality",
		PropertyInfo( Variant::INT, "far_surface_response_quality", PROPERTY_HINT_ENUM, "Simple,Detailed" ),
		"set_far_surface_response_quality",
		"get_far_surface_response_quality" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setFarTextureSurfaceMode,
					   getFarTextureSurfaceMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "far_texture_surface_mode",
									 PROPERTY_HINT_ENUM,
									 "Textured,Silhouette / No Textures" ),
					   "set_far_texture_surface_mode",
					   "get_far_texture_surface_mode" );
	ADD_SUBGROUP( "Lighting & Texture Blending", "" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarLightingModel,
		getFarLightingModel,
		"model",
		PropertyInfo( Variant::INT, "far_lighting_model", PROPERTY_HINT_ENUM, "Unshaded,Lambert,Blinn-Phong,PBR" ),
		"set_far_lighting_model",
		"get_far_lighting_model" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setFarDetilingQuality,
					   getFarDetilingQuality,
					   "quality",
					   PropertyInfo( Variant::INT, "far_detiling_quality", PROPERTY_HINT_ENUM, "Disabled,Rotation" ),
					   "set_far_detiling_quality",
					   "get_far_detiling_quality" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarAuthoredColorContribution,
		getFarAuthoredColorContribution,
		"contribution",
		PropertyInfo( Variant::FLOAT, "far_authored_color_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
		"set_far_authored_color_contribution",
		"get_far_authored_color_contribution" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setFarWetnessContribution,
					   getFarWetnessContribution,
					   "contribution",
					   PropertyInfo( Variant::FLOAT, "far_wetness_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
					   "set_far_wetness_contribution",
					   "get_far_wetness_contribution" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setFarMaterialLodBias,
					   getFarMaterialLodBias,
					   "bias",
					   PropertyInfo( Variant::FLOAT, "far_material_lod_bias", PROPERTY_HINT_RANGE, "-2,4,0.25" ),
					   "set_far_material_lod_bias",
					   "get_far_material_lod_bias" );

	ADD_GROUP( "Projection & Surface Variation", "" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setProjectionMode,
					   getProjectionMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "projection_mode",
									 PROPERTY_HINT_ENUM,
									 "Disabled,Shared Fragment Basis,Exact Per-Corner" ),
					   "set_projection_mode",
					   "get_projection_mode" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setTessellationMaterialMode,
					   getTessellationMaterialMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "tessellation_material_mode",
									 PROPERTY_HINT_ENUM,
									 "Geometry Only,Geometry + Material Height" ),
					   "set_tessellation_material_mode",
					   "get_tessellation_material_mode" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setDualScalingEnabled,
					   getDualScalingEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "dual_scaling_enabled" ),
					   "set_dual_scaling_enabled",
					   "get_dual_scaling_enabled" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setMacroVariationEnabled,
					   getMacroVariationEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "macro_variation_enabled" ),
					   "set_macro_variation_enabled",
					   "get_macro_variation_enabled" );

	ADD_GROUP( "Output Channels & Debug", "" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setOutputAlbedoEnabled,
					   getOutputAlbedoEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "output_albedo" ),
					   "set_output_albedo_enabled",
					   "get_output_albedo_enabled" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setOutputRoughnessEnabled,
					   getOutputRoughnessEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "output_roughness" ),
					   "set_output_roughness_enabled",
					   "get_output_roughness_enabled" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setOutputSpecularEnabled,
					   getOutputSpecularEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "output_specular" ),
					   "set_output_specular_enabled",
					   "get_output_specular_enabled" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setOutputNormalMapEnabled,
					   getOutputNormalMapEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "output_normal_map" ),
					   "set_output_normal_map_enabled",
					   "get_output_normal_map_enabled" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setOutputAmbientOcclusionEnabled,
					   getOutputAmbientOcclusionEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "output_ambient_occlusion" ),
					   "set_output_ambient_occlusion_enabled",
					   "get_output_ambient_occlusion_enabled" );

	ADD_GROUP( "Shader Status (Read Only)", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"near_range_context",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_near_range_context" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"near_shadow_context",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_near_shadow_context" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"near_shader_status",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_near_shader_status" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"middle_range_context",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_middle_range_context" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"middle_shadow_context",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_middle_shadow_context" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"middle_shader_status",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_middle_shader_status" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"far_range_context",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_far_range_context" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"far_shadow_context",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_far_shadow_context" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"far_shader_status",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_far_shader_status" );

	ADD_GROUP( "Serialized Compatibility", "" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setTextureFiltering,
		getTextureFiltering,
		"filtering",
		PropertyInfo( Variant::INT, "texture_filtering", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_texture_filtering",
		"get_texture_filtering" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setTerrainSamplingMode,
		getTerrainSamplingMode,
		"mode",
		PropertyInfo( Variant::INT, "terrain_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_terrain_sampling_mode",
		"get_terrain_sampling_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setTerrainNormalMode,
		getTerrainNormalMode,
		"mode",
		PropertyInfo( Variant::INT, "terrain_normal_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_terrain_normal_mode",
		"get_terrain_normal_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setColorSamplingMode,
		getColorSamplingMode,
		"mode",
		PropertyInfo( Variant::INT, "color_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_color_sampling_mode",
		"get_color_sampling_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setSurfaceDataMode,
		getSurfaceDataMode,
		"mode",
		PropertyInfo( Variant::INT, "surface_data_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_surface_data_mode",
		"get_surface_data_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setTextureSurfaceMode,
		getTextureSurfaceMode,
		"mode",
		PropertyInfo( Variant::INT, "texture_surface_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_texture_surface_mode",
		"get_texture_surface_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setTerrainShadingMode,
		getTerrainShadingMode,
		"mode",
		PropertyInfo( Variant::INT, "terrain_shading_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_terrain_shading_mode",
		"get_terrain_shading_mode" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setDetilingMode,
					   getDetilingMode,
					   "mode",
					   PropertyInfo( Variant::INT, "detiling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
					   "set_detiling_mode",
					   "get_detiling_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleTerrainSamplingMode,
		getMiddleTerrainSamplingMode,
		"mode",
		PropertyInfo( Variant::INT, "middle_terrain_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_terrain_sampling_mode",
		"get_middle_terrain_sampling_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleTerrainNormalMode,
		getMiddleTerrainNormalMode,
		"mode",
		PropertyInfo( Variant::INT, "middle_terrain_normal_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_terrain_normal_mode",
		"get_middle_terrain_normal_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleColorSamplingMode,
		getMiddleColorSamplingMode,
		"mode",
		PropertyInfo( Variant::INT, "middle_color_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_color_sampling_mode",
		"get_middle_color_sampling_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleSurfaceDataMode,
		getMiddleSurfaceDataMode,
		"mode",
		PropertyInfo( Variant::INT, "middle_surface_data_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_surface_data_mode",
		"get_middle_surface_data_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleTerrainShadingMode,
		getMiddleTerrainShadingMode,
		"mode",
		PropertyInfo( Variant::INT, "middle_terrain_shading_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_terrain_shading_mode",
		"get_middle_terrain_shading_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleDetilingMode,
		getMiddleDetilingMode,
		"mode",
		PropertyInfo( Variant::INT, "middle_detiling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_detiling_mode",
		"get_middle_detiling_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleAutoShaderEnabled,
		getMiddleAutoShaderEnabled,
		"enabled",
		PropertyInfo( Variant::BOOL, "middle_auto_shader_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_auto_shader_enabled",
		"get_middle_auto_shader_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleAuthoredColorEnabled,
		getMiddleAuthoredColorEnabled,
		"enabled",
		PropertyInfo( Variant::BOOL, "middle_authored_color_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_authored_color_enabled",
		"get_middle_authored_color_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setMiddleWetnessEnabled,
		getMiddleWetnessEnabled,
		"enabled",
		PropertyInfo( Variant::BOOL, "middle_wetness_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_middle_wetness_enabled",
		"get_middle_wetness_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarTerrainShadingMode,
		getFarTerrainShadingMode,
		"mode",
		PropertyInfo( Variant::INT, "far_terrain_shading_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_far_terrain_shading_mode",
		"get_far_terrain_shading_mode" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarMaterialLod,
		getFarMaterialLod,
		"lod",
		PropertyInfo( Variant::FLOAT, "far_material_lod", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_far_material_lod",
		"get_far_material_lod" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarAuthoredColorEnabled,
		getFarAuthoredColorEnabled,
		"enabled",
		PropertyInfo( Variant::BOOL, "far_authored_color_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_far_authored_color_enabled",
		"get_far_authored_color_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setFarWetnessEnabled,
		getFarWetnessEnabled,
		"enabled",
		PropertyInfo( Variant::BOOL, "far_wetness_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_far_wetness_enabled",
		"get_far_wetness_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setPackedPbrSingleFetch,
		getPackedPbrSingleFetch,
		"enabled",
		PropertyInfo( Variant::BOOL, "packed_pbr_single_fetch", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_packed_pbr_single_fetch",
		"get_packed_pbr_single_fetch" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setProjectionEnabled,
		getProjectionEnabled,
		"enabled",
		PropertyInfo( Variant::BOOL, "projection_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_projection_enabled",
		"get_projection_enabled" );

	ADD_GROUP( "Hidden Terrain Aliases", "" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowRegionGrid,
		getShowRegionGrid,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_region_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_region_grid",
		"get_show_region_grid" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowInstancerGrid,
		getShowInstancerGrid,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_instancer_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_instancer_grid",
		"get_show_instancer_grid" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowVertexGrid,
		getShowVertexGrid,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_vertex_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_vertex_grid",
		"get_show_vertex_grid" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setShowContours,
					   getShowContours,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_contours", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
					   "set_show_contours",
					   "get_show_contours" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowNavigation,
		getShowNavigation,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_navigation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_navigation",
		"get_show_navigation" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setDisplacementScale,
		getDisplacementScale,
		"scale",
		PropertyInfo( Variant::FLOAT, "displacement_scale", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_displacement_scale",
		"get_displacement_scale" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setDisplacementSharpness,
		getDisplacementSharpness,
		"sharpness",
		PropertyInfo( Variant::FLOAT, "displacement_sharpness", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_displacement_sharpness",
		"get_displacement_sharpness" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowCheckered,
		getShowCheckered,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_checkered", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_checkered",
		"get_show_checkered" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setShowGrey,
					   getShowGrey,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_grey", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
					   "set_show_grey",
					   "get_show_grey" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowHeightmap,
		getShowHeightmap,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_heightmap", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_heightmap",
		"get_show_heightmap" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowJaggedness,
		getShowJaggedness,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_jaggedness", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_jaggedness",
		"get_show_jaggedness" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowAutoshader,
		getShowAutoshader,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_autoshader", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_autoshader",
		"get_show_autoshader" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowControlTexture,
		getShowControlTexture,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_control_texture", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_control_texture",
		"get_show_control_texture" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowControlBlend,
		getShowControlBlend,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_control_blend", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_control_blend",
		"get_show_control_blend" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowControlAngle,
		getShowControlAngle,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_control_angle", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_control_angle",
		"get_show_control_angle" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowControlScale,
		getShowControlScale,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_control_scale", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_control_scale",
		"get_show_control_scale" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setShowColormap,
					   getShowColormap,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_colormap", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
					   "set_show_colormap",
					   "get_show_colormap" );
	T3D_BIND_PROPERTY( Terrain3DMaterial,
					   setShowRoughmap,
					   getShowRoughmap,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_roughmap", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
					   "set_show_roughmap",
					   "get_show_roughmap" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowTextureAlbedo,
		getShowTextureAlbedo,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_texture_albedo", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_texture_albedo",
		"get_show_texture_albedo" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowTextureHeight,
		getShowTextureHeight,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_texture_height", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_texture_height",
		"get_show_texture_height" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowTextureNormal,
		getShowTextureNormal,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_texture_normal", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_texture_normal",
		"get_show_texture_normal" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowTextureAo,
		getShowTextureAo,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_texture_ao", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_texture_ao",
		"get_show_texture_ao" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowTextureRough,
		getShowTextureRough,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_texture_rough", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_texture_rough",
		"get_show_texture_rough" );
	T3D_BIND_PROPERTY(
		Terrain3DMaterial,
		setShowDisplacementBuffer,
		getShowDisplacementBuffer,
		"enabled",
		PropertyInfo( Variant::BOOL, "show_displacement_buffer", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
		"set_show_displacement_buffer",
		"get_show_displacement_buffer" );

	ADD_SIGNAL( MethodInfo( "shader_tier_state_changed",
							PropertyInfo( Variant::INT, "tier" ),
							PropertyInfo( Variant::STRING, "variant_signature" ),
							PropertyInfo( Variant::BOOL, "compile_valid" ),
							PropertyInfo( Variant::BOOL, "fallback_to_near" ) ) );
}

// -------- Consolidated from rendering/material/terrain_3d_shader_specializer.cpp --------
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
bool t3dTerrainShaderPipelineKeyIsValid(
	const t3dTerrainShaderPipelineKey_t &rKey ) {
	if ( rKey.m_nPassClass >= T3D_TERRAIN_SHADER_PASS_COUNT || rKey.m_nTier >= 3u ||
		 rKey.m_nShaderPath >= T3D_TERRAIN_SHADER_PATH_COUNT ||
		 rKey.m_nMaterialBackend >= T3D_TERRAIN_MATERIAL_BACKEND_COUNT ) {
		return false;
	}
	const bool bColorPass = rKey.m_nPassClass == T3D_TERRAIN_SHADER_PASS_COLOR_GENERAL ||
							rKey.m_nPassClass == T3D_TERRAIN_SHADER_PASS_COLOR_ONE_SUN;
	if ( rKey.m_nEditorDiagnosticClass >= T3D_TERRAIN_EDITOR_DIAGNOSTIC_CLASS_COUNT ||
		 rKey.m_nNoiseSourceClass >= T3D_TERRAIN_NOISE_SOURCE_CLASS_COUNT ||
		 ( rKey.m_nShaderPath != T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED &&
		   rKey.m_nEditorDiagnosticClass != T3D_TERRAIN_EDITOR_DIAGNOSTIC_RUNTIME ) ||
		 ( !bColorPass &&
		   rKey.m_nEditorDiagnosticClass != T3D_TERRAIN_EDITOR_DIAGNOSTIC_RUNTIME ) ) {
		return false;
	}
	if ( rKey.m_nShaderPath == T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL ) {
		if ( ( rKey.m_nMaterialBackend != T3D_TERRAIN_MATERIAL_BACKEND_CANONICAL_VT_V1 &&
               rKey.m_nMaterialBackend != T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1 ) ||
			 ( rKey.m_nShippingFeatureClass & ~T3D_TERRAIN_SHIPPING_FEATURE_MASK ) != 0u ) {
			return false;
		}
		const bool bDnrPass = rKey.m_nPassClass == T3D_TERRAIN_SHADER_PASS_DEPTH_NORMAL_ROUGHNESS;
		const bool bDnrValid = bDnrPass ? rKey.m_nDnrOutputClass < T3D_TERRAIN_DNR_OUTPUT_CLASS_COUNT
										: rKey.m_nDnrOutputClass == T3D_TERRAIN_DNR_OUTPUT_RUNTIME;
		const bool bMiddleValid = rKey.m_nTier == 1u
									  ? rKey.m_nMiddleSurfaceClass < T3D_TERRAIN_MIDDLE_SURFACE_CLASS_COUNT
									  : rKey.m_nMiddleSurfaceClass == T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;
		if ( !bDnrValid || !bMiddleValid ) {
			return false;
		}
		if ( bColorPass ) {
			return rKey.m_nLightingModelClass < T3D_TERRAIN_LIGHTING_MODEL_CLASS_COUNT &&
				   rKey.m_nSceneIntegrationClass < T3D_TERRAIN_SCENE_INTEGRATION_CLASS_COUNT;
		}
		return rKey.m_nLightingModelClass == T3D_TERRAIN_LIGHTING_MODEL_RUNTIME &&
			   rKey.m_nSceneIntegrationClass == T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME;
	}
	return rKey.m_nMaterialBackend == T3D_TERRAIN_MATERIAL_BACKEND_COMPATIBILITY_MIXED &&
		   rKey.m_nShippingFeatureClass == T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME &&
		   rKey.m_nLightingModelClass == T3D_TERRAIN_LIGHTING_MODEL_RUNTIME &&
		   rKey.m_nSceneIntegrationClass == T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME &&
		   rKey.m_nDnrOutputClass == T3D_TERRAIN_DNR_OUTPUT_RUNTIME &&
		   rKey.m_nMiddleSurfaceClass == T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;
}
