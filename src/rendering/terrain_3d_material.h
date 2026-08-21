// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MATERIAL_CLASS_H
#define TERRAIN3D_MATERIAL_CLASS_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "eastl_config.h"

#include <EASTL/vector.h>
#include <cstdint>
#include <godot_cpp/classes/noise_texture2d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

class T3DTerrain;

typedef enum t3dWorldBackground_e {
	NONE,
	FLAT,
	NOISE,
} t3dWorldBackground_t;

typedef enum t3dTextureFiltering_e {
	LINEAR_ANISOTROPIC,
	LINEAR,
	NEAREST_ANISOTROPIC,
	NEAREST,
} t3dTextureFiltering_t;

typedef enum t3dUpdateFlags_e {
	UNIFORMS_ONLY = 0,
	TEXTURE_ARRAYS = 1 << 0,
	REGION_ARRAYS = 1 << 1,
	UPDATE_ARRAYS = TEXTURE_ARRAYS | REGION_ARRAYS,
	FULL_REBUILD = ( 1 << 2 ) | UPDATE_ARRAYS,
} t3dUpdateFlags_t;

typedef enum t3dShaderVariantMode_e {
	SHADER_VARIANTS_DISABLED,
	SHADER_VARIANTS_ON_DEMAND,
	// Prewarms the current shader plus a bounded set of immediately plausible
	// alternatives. Unused speculative variants are evicted before used ones.
	SHADER_VARIANTS_PREWARM_ACTIVE_BRANCHES,
	// Adds alternatives from shader dimensions that were actually changed in
	// this material's current session. This is the default usage-driven mode.
	SHADER_VARIANTS_PREWARM_ACTIVE_AND_RECENT,
	// Serialized compatibility alias for the former broad feature-matrix mode.
	// It now uses bounded active-and-recent prewarming instead of a Cartesian set.
	SHADER_VARIANTS_PREWARM_FEATURE_MATRIX = SHADER_VARIANTS_PREWARM_ACTIVE_AND_RECENT,
	// Serialized compatibility alias for the previous mode name/value.
	SHADER_VARIANTS_PREWARM_COMMON = SHADER_VARIANTS_PREWARM_ACTIVE_BRANCHES,
} t3dShaderVariantMode_t;

typedef enum t3dShaderPerformancePreset_e {
	SHADER_PRESET_CUSTOM,
	SHADER_PRESET_CINEMATIC,
	SHADER_PRESET_HIGH,
	SHADER_PRESET_RUNTIME_360,
	SHADER_PRESET_LOW,
	SHADER_PRESET_MINIMUM,
	// Appended so existing serialized preset values remain stable.
	SHADER_PRESET_XBOX_360_AUTHENTIC,
	// Serialized compatibility aliases.
	SHADER_PRESET_BRANCHLESS_QUALITY = SHADER_PRESET_CINEMATIC,
	SHADER_PRESET_XBOX_360_STYLED = SHADER_PRESET_RUNTIME_360,
	SHADER_PRESET_XBOX_360 = SHADER_PRESET_RUNTIME_360,
	SHADER_PRESET_FASTEST = SHADER_PRESET_MINIMUM,
} t3dShaderPerformancePreset_t;

typedef enum t3dShaderTier_e {
	SHADER_TIER_NEAR,
	SHADER_TIER_MIDDLE,
	SHADER_TIER_FAR,
	SHADER_TIER_COUNT,
} t3dShaderTier_t;

typedef enum t3dTerrainSamplingMode_e {
	TERRAIN_SAMPLING_ADAPTIVE,
	TERRAIN_SAMPLING_ALWAYS_SMOOTH,
	TERRAIN_SAMPLING_SINGLE_SAMPLE,
	// Appended so existing serialized values remain stable.
	TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED,
	TERRAIN_SAMPLING_DISTANCE_ADAPTIVE,
	TERRAIN_SAMPLING_ADAPTIVE_2_CORNER,
} t3dTerrainSamplingMode_t;

typedef enum t3dTerrainNormalMode_e {
	TERRAIN_NORMAL_ADAPTIVE,
	TERRAIN_NORMAL_HEIGHT_8_TAP,
	TERRAIN_NORMAL_GEOMETRY_DERIVATIVE,
	TERRAIN_NORMAL_DERIVED_TEXTURE,
	TERRAIN_NORMAL_HEIGHT_3_TAP,
	// Serialized compatibility aliases.
	TERRAIN_NORMAL_SMOOTH = TERRAIN_NORMAL_HEIGHT_8_TAP,
	TERRAIN_NORMAL_FLAT = TERRAIN_NORMAL_GEOMETRY_DERIVATIVE,
} t3dTerrainNormalMode_t;

typedef enum t3dColorSamplingMode_e {
	COLOR_SAMPLING_AUTO,
	COLOR_SAMPLING_HARDWARE_FILTERED,
	COLOR_SAMPLING_EXACT_4_TAP,
} t3dColorSamplingMode_t;

typedef enum t3dProjectionMode_e {
	PROJECTION_DISABLED,
	PROJECTION_SHARED_FRAGMENT,
	PROJECTION_EXACT_PER_CORNER,
} t3dProjectionMode_t;

typedef enum t3dTessellationMaterialMode_e {
	TESSELLATION_GEOMETRY_ONLY,
	TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT,
} t3dTessellationMaterialMode_t;

typedef enum t3dSurfaceDataMode_e {
	SURFACE_DATA_AUTO,
	SURFACE_DATA_FULL,
	SURFACE_DATA_ALBEDO_ONLY,
} t3dSurfaceDataMode_t;

typedef enum t3dTextureSurfaceMode_e {
	TEXTURE_SURFACE_TEXTURED,
	TEXTURE_SURFACE_SILHOUETTE,
} t3dTextureSurfaceMode_t;

typedef enum t3dTerrainShadingMode_e {
	SHADING_BURLEY_GGX,
	SHADING_LAMBERT_GGX,
	SHADING_LAMBERT_DIFFUSE_ONLY,
	SHADING_UNSHADED,
	// Appended to preserve serialized values of the original four modes.
	SHADING_LAMBERT_VERTEX,
	// Non-PBR Lambert + Blinn-Phong response approximating common 2005-2013 terrain renderers.
	SHADING_XBOX_360_BLINN_PHONG,
} t3dTerrainShadingMode_t;

typedef enum t3dMaterialSamplingQuality_e {
	MATERIAL_SAMPLING_FAST,
	MATERIAL_SAMPLING_FULL,
} t3dMaterialSamplingQuality_t;

typedef enum t3dMiddleSurfaceSource_e {
	MIDDLE_SURFACE_BAKED_COMPOSITE,
	MIDDLE_SURFACE_RUNTIME_MATERIAL,
} t3dMiddleSurfaceSource_t;

typedef enum t3dTerrainNormalQuality_e {
	TERRAIN_NORMAL_QUALITY_GEOMETRY,
	TERRAIN_NORMAL_QUALITY_DETAILED,
} t3dTerrainNormalQuality_t;

typedef enum t3dTerrainColorQuality_e {
	TERRAIN_COLOR_QUALITY_FAST,
	TERRAIN_COLOR_QUALITY_EXACT,
} t3dTerrainColorQuality_t;

typedef enum t3dSurfaceResponseQuality_e {
	SURFACE_RESPONSE_SIMPLE,
	SURFACE_RESPONSE_DETAILED,
} t3dSurfaceResponseQuality_t;

typedef enum t3dTierLightingModel_e {
	TIER_LIGHTING_UNSHADED,
	TIER_LIGHTING_LAMBERT,
	TIER_LIGHTING_BLINN_PHONG,
	TIER_LIGHTING_PBR,
} t3dTierLightingModel_t;

typedef enum t3dTierDetilingQuality_e {
	TIER_DETILING_DISABLED,
	TIER_DETILING_ROTATION,
} t3dTierDetilingQuality_t;

typedef enum t3dDetilingMode_e {
	DETILING_CONTINUOUS,
	DETILING_DISCRETE_8,
	DETILING_DISABLED,
	// Appended so existing serialized values remain stable.
	DETILING_TRANSLATION_ONLY,
	DETILING_DISCRETE_4,
	DETILING_DISCRETE_16,
} t3dDetilingMode_t;

typedef enum t3dWorldNoiseShaderMode_e {
	WORLD_NOISE_DYNAMIC,
	WORLD_NOISE_CACHED_VERTEX_NORMALS,
	WORLD_NOISE_CACHED_FRAGMENT_NORMALS,
	WORLD_NOISE_PROCEDURAL_VERTEX_NORMALS,
	WORLD_NOISE_PROCEDURAL_FRAGMENT_NORMALS,
} t3dWorldNoiseShaderMode_t;

typedef enum t3dShaderVariantDomain_e {
	SHADER_DOMAIN_RUNTIME,
	SHADER_DOMAIN_EDITOR,
	SHADER_DOMAIN_BUFFER,
} t3dShaderVariantDomain_t;

typedef enum t3dShaderVariantDimension_e {
	VARIANT_DIM_WORLD_BACKGROUND,
	VARIANT_DIM_TEXTURE_FILTERING,
	VARIANT_DIM_TERRAIN_SAMPLING,
	VARIANT_DIM_TERRAIN_NORMAL,
	VARIANT_DIM_COLOR_SAMPLING,
	VARIANT_DIM_PROJECTION,
	VARIANT_DIM_TESSELLATION_MATERIAL,
	VARIANT_DIM_SURFACE_DATA,
	VARIANT_DIM_SHADING,
	VARIANT_DIM_DETILING,
	VARIANT_DIM_WORLD_NOISE,
	VARIANT_DIM_AUTO_SHADER,
	VARIANT_DIM_DUAL_SCALING,
	VARIANT_DIM_MACRO_VARIATION,
	VARIANT_DIM_OUTPUT_ALBEDO,
	VARIANT_DIM_OUTPUT_ROUGHNESS,
	VARIANT_DIM_OUTPUT_SPECULAR,
	VARIANT_DIM_OUTPUT_NORMAL,
	VARIANT_DIM_OUTPUT_AO,
	VARIANT_DIM_TEXTURE_SURFACE,
	VARIANT_DIM_COUNT,
} t3dShaderVariantDimension_t;

typedef struct t3dShaderVariantState_s {
	t3dWorldBackground_t m_worldBackground = FLAT;
	t3dTextureFiltering_t m_textureFiltering = LINEAR;
	t3dTerrainSamplingMode_t m_terrainSamplingMode = TERRAIN_SAMPLING_ADAPTIVE;
	t3dTerrainNormalMode_t m_terrainNormalMode = TERRAIN_NORMAL_ADAPTIVE;
	t3dColorSamplingMode_t m_colorSamplingMode = COLOR_SAMPLING_AUTO;
	t3dProjectionMode_t m_projectionMode = PROJECTION_DISABLED;
	t3dTessellationMaterialMode_t m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
	t3dSurfaceDataMode_t m_surfaceDataMode = SURFACE_DATA_AUTO;
	t3dTextureSurfaceMode_t m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	t3dTerrainShadingMode_t m_terrainShadingMode = SHADING_LAMBERT_GGX;
	t3dDetilingMode_t m_detilingMode = DETILING_CONTINUOUS;
	t3dWorldNoiseShaderMode_t m_worldNoiseShaderMode = WORLD_NOISE_DYNAMIC;
	bool m_bAutoShaderEnabled = false;
	bool m_bDualScalingEnabled = false;
	bool m_bMacroVariationEnabled = false;
	bool m_bOutputAlbedoEnabled = true;
	bool m_bOutputRoughnessEnabled = true;
	bool m_bOutputSpecularEnabled = true;
	bool m_bOutputNormalMapEnabled = true;
	bool m_bOutputAmbientOcclusionEnabled = true;
	bool m_bFastBlendCurve = false;
	bool m_bTessellationEnabled = false;
	bool m_bSurfacePacked = false;
	bool m_bControlMapsImplicit = false;
	bool m_bColorMapsImplicit = false;
} t3dShaderVariantState_t;

typedef struct t3dTierQualityConfig_s {
	t3dTextureFiltering_t m_textureFiltering = LINEAR;
	t3dMaterialSamplingQuality_t m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
	t3dTerrainNormalQuality_t m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
	t3dTerrainColorQuality_t m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
	t3dSurfaceResponseQuality_t m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
	t3dTextureSurfaceMode_t m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	t3dTierLightingModel_t m_lightingModel = TIER_LIGHTING_LAMBERT;
	t3dTierDetilingQuality_t m_detilingQuality = TIER_DETILING_DISABLED;
	real_t m_fAuthoredColorContribution = 1.0f;
	real_t m_fWetnessContribution = 1.0f;
	real_t m_fMaterialLodBias = 0.0f;
} t3dTierQualityConfig_t;

typedef struct t3dShaderVariantKey_s {
	uint64_t m_nPrimaryHash = 0;
	uint64_t m_nFingerprint = 0;
	uint32_t m_nSourceLength = 0;

	bool operator==( const t3dShaderVariantKey_s &rOther ) const {
		return m_nPrimaryHash == rOther.m_nPrimaryHash && m_nFingerprint == rOther.m_nFingerprint &&
				m_nSourceLength == rOther.m_nSourceLength;
	}
} t3dShaderVariantKey_t;

typedef struct t3dShaderValidationResult_s {
	bool m_bValid = false;
	int m_nReflectedParameterCount = 0;
	uint64_t m_nShaderRidId = 0u;
	String m_failureSummary;
} t3dShaderValidationResult_t;

typedef struct t3dShaderTierValidationState_s {
	t3dShaderVariantKey_t m_key;
	uint64_t m_nRequestedShaderRidId = 0u;
	uint64_t m_nActiveShaderRidId = 0u;
	bool m_bCompileValid = false;
	int m_nReflectedParameterCount = 0;
	String m_failureSummary;
	bool m_bFallbackToNear = false;
	bool m_bCompactTierActive = false;
} t3dShaderTierValidationState_t;

typedef struct t3dShaderVariantEntry_s {
	t3dShaderVariantKey_t m_key;
	Ref<Shader> m_shader;
	uint32_t m_nSourceBytes = 0;
	int m_nReflectedParameterCount = 0;
	bool m_bSpeculative = true;
} t3dShaderVariantEntry_t;

typedef enum t3dPageTableBuildResult_e {
	PAGE_TABLE_READY,
	PAGE_TABLE_INVALID_TERRAIN_STATE,
	PAGE_TABLE_INVALID_DIMENSIONS,
	PAGE_TABLE_TEXTURE_ALLOCATION_FAILED,
} t3dPageTableBuildResult_t;

class T3DMaterial : public Resource {
	GDCLASS( T3DMaterial, Resource );
	CLASS_NAME();

	bool m_bPersistenceDirty = false;

public: // Constants
	// Controls the compile-time shader permutation cache. Feature modules
	// are gated by preprocessor defines and resolved through standard shader includes.

	// High-level profiles exposed by the Terrain3D editor sidebar. CUSTOM keeps
	// every individual hotpath control available. The other profiles assign a
	// bounded group of compile-time modes and then compile/cache that exact path.

	// Independently compiled clipmap shader tiers. These are internal runtime
	// materials, not user-facing material resources. Near preserves the selected
	// surface model, middle resolves only control/color/derived-normal data, and
	// far uses the coarse world atlases.

	// Controls terrain control-map and normal interpolation. Static modes remove
	// the distance-dependent bilinear branch from the generated fragment shader.

	// Controls the final terrain geometric normal source. Static modes remove the
	// public flat_terrain_normals uniform branch.

	// Controls regional color/wetness filtering. AUTO uses one hardware-filtered
	// sample inside a region and exact four-corner sampling only at layer edges.

	// Controls steep-surface UV projection. SHARED uses the final fragment terrain
	// normal for all material corners; EXACT evaluates each corner independently.

	// Controls whether tessellation displacement is sampled again in the fragment
	// material path for height blending/AO suppression.

	// Controls whether the material surface array is fetched. AUTO retains the
	// fetch only when an enabled PBR output consumes normal/roughness/AO data.

	// Controls whether terrain texture arrays participate in the surface pass.
	// SILHOUETTE preserves all authored texture/control data while compiling the
	// material texture sampling path out of the terrain shaders.

	// Selects compile-time Godot spatial render modes and lighting frequency.

	// Near, middle, and far rendering share one semantic quality schema.
	// Each tier maps these choices onto a tier-appropriate implementation rather
	// than exposing the legacy Near shader internals directly.

	// Detiling quality/cost. DISCRETE_8 replaces the sine hash plus per-material
	// sin/cos pair with an integer hash and an eight-entry rotation table.

	// Dynamic preserves the public uniforms. The static modes remove both world
	// noise source branches from the compiled shader.

private:
	T3DTerrain *m_pTerrain = nullptr;

	RID m_hMaterial;
	RID m_hszTierMaterials[SHADER_TIER_COUNT] = {};
	RID m_hszShadowMaterials[SHADER_TIER_COUNT] = {};
	Ref<Shader> m_shader; // Active near shader
	bool m_bNearShaderReady = false;
	Ref<Shader> m_szTierShaders[SHADER_TIER_COUNT];
	Ref<Shader> m_shadowShader;
	Dictionary m_shaderTemplates; // External root shader templates loaded from the addon
	bool m_bRuntimeShaderPackLoaded = false;
	uint64_t m_nRuntimeShaderPackStateSignature = 0u;
	String m_szRuntimeShaderPackTierSource[SHADER_TIER_COUNT];
	String m_runtimeShaderPackBufferSource;
	t3dShaderVariantEntry_t m_szRuntimeShaderVariantEntries[s_nMaxShaderVariantCacheLimit] = {};
	t3dShaderVariantEntry_t m_szEditorShaderVariantEntries[s_nMaxEditorShaderVariantCacheLimit] = {};
	int m_nRuntimeShaderVariantCount = 0;
	int m_nEditorShaderVariantCount = 0;
	uint64_t m_nRuntimeShaderVariantSourceBytes = 0;
	uint64_t m_nEditorShaderVariantSourceBytes = 0;
	bool m_bShaderOverrideEnabled = false;
	Ref<Shader> m_shaderOverride; // User's shader we copy code from
	Ref<Shader> m_connectedShaderOverride;
	mutable TypedArray<StringName> m_activeParams; // All public shader params in the current shader
	mutable Dictionary m_shaderParams; // Public shader params saved to disk
	eastl::vector<PropertyInfo> m_shaderPropertyCache;
	Dictionary m_shaderParamDefaults;
	// Mirrors values pushed to private/runtime uniforms so hot callers never read
	// RenderingServer material state back to the CPU.
	mutable Dictionary m_runtimeMaterialParams;
	uint64_t m_nShaderParameterListQueryCount = 0u;
	uint64_t m_nShaderParameterDefaultQueryCount = 0u;
	mutable uint64_t m_nShaderInspectorRuntimeReadCount = 0u;

	RID m_hBufferMaterial;
	Ref<Shader> m_bufferShader; // Active buffer shader
	t3dShaderVariantEntry_t m_szBufferShaderVariantEntries[s_nMaxBufferShaderVariantCacheLimit] = {};
	int m_nBufferShaderVariantCount = 0;
	uint64_t m_nBufferShaderVariantSourceBytes = 0;
	bool m_bBufferShaderOverrideEnabled = false;
	Ref<Shader> m_bufferShaderOverride; // User's shader we copy code from
	Ref<Shader> m_connectedBufferShaderOverride;
	real_t m_fDisplacementScale = 1.0f;
	real_t m_fDisplacementSharpness = 0.5f;
	T3DGeneratedTexture m_generatedDummy;
	T3DGeneratedTexture m_generatedFarHeightDummy;
	T3DGeneratedTexture m_generatedFarColorDummy;
	T3DGeneratedTexture m_generatedSurfaceDummy;
	T3DGeneratedTexture m_generatedDisplacementDummy;
	T3DGeneratedTexture m_generatedRegionMap;
	uint64_t m_nRegionMapTextureHash = 0u;
	int m_nRegionMapTextureSize = 0;
	T3DGeneratedTexture m_generatedLogicalRegionMap;
	uint64_t m_nLogicalRegionMapTextureHash = 0u;
	int m_nLogicalRegionMapTextureSize = 0;
	uint32_t m_nRegionPageTableOwnerGeneration = 0u;
	Ref<NoiseTexture2D> m_worldNoiseCache;

	// Material Features
	t3dWorldBackground_t m_worldBackground = FLAT;
	t3dShaderVariantMode_t m_shaderVariantMode = SHADER_VARIANTS_PREWARM_ACTIVE_AND_RECENT;
	t3dShaderPerformancePreset_t m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	t3dProjectionMode_t m_projectionMode = PROJECTION_DISABLED;
	t3dTessellationMaterialMode_t m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
	t3dWorldNoiseShaderMode_t m_worldNoiseShaderMode = WORLD_NOISE_DYNAMIC;
	bool m_bDualScalingEnabled = false;
	bool m_bAutoShaderEnabled = false;
	bool m_bMacroVariationEnabled = false;
	t3dTierQualityConfig_t m_nearQuality = { LINEAR,
		MATERIAL_SAMPLING_FULL,
		TERRAIN_NORMAL_QUALITY_DETAILED,
		TERRAIN_COLOR_QUALITY_EXACT,
		SURFACE_RESPONSE_DETAILED,
		TEXTURE_SURFACE_TEXTURED,
		TIER_LIGHTING_PBR,
		TIER_DETILING_ROTATION,
		1.0f,
		1.0f,
		0.0f };
	t3dTierQualityConfig_t m_middleQuality = { LINEAR,
		MATERIAL_SAMPLING_FULL,
		TERRAIN_NORMAL_QUALITY_DETAILED,
		TERRAIN_COLOR_QUALITY_FAST,
		SURFACE_RESPONSE_SIMPLE,
		TEXTURE_SURFACE_TEXTURED,
		TIER_LIGHTING_LAMBERT,
		TIER_DETILING_ROTATION,
		1.0f,
		1.0f,
		0.0f };
	t3dTierQualityConfig_t m_farQuality = { LINEAR,
		MATERIAL_SAMPLING_FULL,
		TERRAIN_NORMAL_QUALITY_GEOMETRY,
		TERRAIN_COLOR_QUALITY_FAST,
		SURFACE_RESPONSE_SIMPLE,
		TEXTURE_SURFACE_TEXTURED,
		TIER_LIGHTING_LAMBERT,
		TIER_DETILING_DISABLED,
		1.0f,
		1.0f,
		-0.5f };
#ifdef DEBUG_ENABLED
	uint64_t m_szTierUniformPublicationCount[SHADER_TIER_COUNT] = {};
#endif
	uint64_t m_nPrewarmSignature = UINT64_MAX;
	int m_nShaderVariantCacheLimit = 32;
	int m_nShaderVariantPrewarmLimit = 8;
	int m_nShaderVariantSourceBudgetMb = 8;
	int m_nShaderVariantEffectiveSourceBudgetMb = 8;
	int m_nLastPrewarmVariantCount = 0;
	int m_nLastPrewarmSkippedCount = 0;
	t3dShaderVariantState_t m_lastBoundVariantState;
	bool m_bHasLastBoundVariantState = false;
	uint64_t m_nObservedVariantDimensions = 0;
	uint64_t m_szVariantDimensionLastUsed[VARIANT_DIM_COUNT] = {};
	uint64_t m_nVariantTransitionClock = 0;
	uint64_t m_nVariantCacheHitsTotal = 0;
	uint64_t m_nVariantCacheMissesTotal = 0;
	uint64_t m_nVariantCreationsTotal = 0;
	uint64_t m_nVariantPrewarmCreationsTotal = 0;
	uint64_t m_nVariantUncachedSubmissionsTotal = 0;
	uint64_t m_nVariantAllocationFailuresTotal = 0;
	uint64_t m_nVariantRemovalsTotal = 0;
	uint64_t m_nVariantSourceSubmissionUsecTotal = 0;
	uint64_t m_nVariantSourceSubmissionUsecPeak = 0;
	uint64_t m_nVariantSourceSubmissionUsecLast = 0;
	uint64_t m_nVariantValidationReflectionQueriesTotal = 0;
	uint64_t m_nVariantValidationFailuresTotal = 0;
	t3dShaderTierValidationState_t m_szShaderTierValidation[SHADER_TIER_COUNT] = {};
#ifdef DEBUG_ENABLED
	int m_nShaderRegressionInvalidTier = -1;
#endif
	uint64_t m_nShaderUpdateUsecTotal = 0;
	uint64_t m_nShaderUpdateUsecPeak = 0;
	uint64_t m_nShaderUpdateUsecLast = 0;
	uint64_t m_nShaderUpdateCount = 0;
	int m_nLastVariantRequestDomain = -1;
	bool m_bLastVariantRequestHit = false;
	bool m_bLastVariantRequestPrewarm = false;
	uint32_t m_nLastVariantRequestSourceBytes = 0;
	// PBR Outputs
	bool m_bOutputAlbedoEnabled = true;
	bool m_bOutputRoughnessEnabled = true;
	bool m_bOutputSpecularEnabled = true;
	bool m_bOutputNormalMapEnabled = true;
	bool m_bOutputAmbientOcclusionEnabled = true;

	// Overlays
	bool m_bShowRegionGrid = false;
	bool m_bShowInstancerGrid = false;
	bool m_bShowVertexGrid = false;
	bool m_bShowContours = false;
	bool m_bShowNavigation = false;

	// Debug Views
	bool m_bDebugViewCheckered = false;
	bool m_bDebugViewGrey = false;
	bool m_bDebugViewHeightmap = false;
	bool m_bDebugViewJaggedness = false;
	bool m_bDebugViewAutoshader = false;
	bool m_bDebugViewControlTexture = false;
	bool m_bDebugViewControlBlend = false;
	bool m_bDebugViewControlAngle = false;
	bool m_bDebugViewControlScale = false;
	bool m_bDebugViewColormap = false;
	bool m_bDebugViewRoughmap = false;
	bool m_bDebugViewDisplacementBuffer = false;

	// PBR Views
	bool m_bPbrViewTexAlbedo = false;
	bool m_bPbrViewTexHeight = false;
	bool m_bPbrViewTexNormal = false;
	bool m_bPbrViewTexAo = false;
	bool m_bPbrViewTexRough = false;

	// Functions
	void preloadShaders( void );
	String prepareShaderSource( const String &rShader,
			const t3dShaderVariantState_t &rState,
			const Array &rExcludes = Array(),
			const t3dShaderTier_t tier = SHADER_TIER_NEAR,
			const bool bIncludeEditorFeatures = false ) const;
	PackedStringArray getEditorShaderFeatures( void ) const;
	t3dShaderVariantState_t captureVariantState( void ) const;
	bool samplesSurfaceData( const t3dShaderVariantState_t &rState ) const;
	void applyPerformancePresetToState( t3dShaderVariantState_t &rState,
			const t3dShaderPerformancePreset_t preset ) const;
	void applyXboxProfileManifestToState( t3dShaderVariantState_t &rState ) const;
	void applyXboxProfileManifestToTiers( void );
	void applyVariantState( const t3dShaderVariantState_t &rState );
	t3dShaderVariantKey_t makeVariantCacheKey( const String &rCode ) const;
	t3dShaderVariantEntry_t *getShaderVariantEntries( const t3dShaderVariantDomain_t domain );
	const t3dShaderVariantEntry_t *getShaderVariantEntries( const t3dShaderVariantDomain_t domain ) const;
	int &getShaderVariantCountForDomain( const t3dShaderVariantDomain_t domain );
	int getShaderVariantCountForDomain( const t3dShaderVariantDomain_t domain ) const;
	uint64_t &getShaderVariantDomainSourceBytes( const t3dShaderVariantDomain_t domain );
	uint64_t getShaderVariantDomainSourceBytes( const t3dShaderVariantDomain_t domain ) const;
	int getVariantPhysicalCapacity( const t3dShaderVariantDomain_t domain ) const;
	int getVariantCacheLimit( const t3dShaderVariantDomain_t domain ) const;
	uint64_t getShaderVariantSourceBytesRaw( void ) const;
	bool isActiveShader( const Ref<Shader> &rShader ) const;
	int findShaderVariant( const t3dShaderVariantDomain_t domain, const t3dShaderVariantKey_t &rKey ) const;
	void eraseShaderVariant( const t3dShaderVariantDomain_t domain, const int iIndex );
	void touchShaderVariant( const t3dShaderVariantDomain_t domain, const int iIndex, const bool bActualUse );
	bool evictLruShaderVariant( const t3dShaderVariantDomain_t domain, const bool bSpeculativeOnly = false );
	bool evictBudgetLruShaderVariant( const t3dShaderVariantDomain_t requestDomain,
			const bool bSpeculativeOnly = false );
	void trimShaderVariantCaches( void );
	void submitShaderSource( const Ref<Shader> &rShader, const String &rCode );
	t3dShaderValidationResult_t validateShaderVariant( const Ref<Shader> &rShader, const t3dShaderTier_t tier );
	void recordShaderTierValidation( const t3dShaderTier_t tier,
			const String &rCode,
			const t3dShaderValidationResult_t &rValidation,
			const uint64_t nActiveShaderRidId,
			const bool bFallbackToNear,
			const bool bCompactTierActive );
	Ref<Shader> getOrCreateShaderVariant( const String &rCode,
			const t3dShaderVariantDomain_t domain,
			const bool bPrewarm = false,
			const t3dShaderTier_t tier = SHADER_TIER_NEAR,
			t3dShaderValidationResult_t *pValidation = nullptr );
	uint64_t getChangedVariantDimensions( const t3dShaderVariantState_t &rPrevious,
			const t3dShaderVariantState_t &rCurrent ) const;
	uint64_t getInferredPotentialDimensions( const t3dShaderVariantState_t &rState ) const;
	void recordVariantTransition( const t3dShaderVariantState_t &rState );
	bool
	appendVariantState( t3dShaderVariantState_t *pStates, int &nCount, const t3dShaderVariantState_t &rState ) const;
	void appendDimensionStates( t3dShaderVariantState_t *pStates,
			int &nCount,
			const t3dShaderVariantState_t &rBase,
			const t3dShaderVariantDimension_t dimension ) const;
	void appendPotentialBranchStates( t3dShaderVariantState_t *pStates,
			int &nCount,
			const t3dShaderVariantState_t &rBase ) const;
	void prewarmShaderVariantsInternal( const bool bForce = false );
	uint64_t getVariantStateSignature( const t3dShaderVariantState_t &rState ) const;
	uint64_t getPrewarmSignature( void ) const;
	String generateShaderCode( void ) const;
	String generateShaderCode( const t3dShaderVariantState_t &rState ) const;
	String generateShaderCode( const t3dShaderVariantState_t &rState,
			const t3dShaderTier_t tier,
			const bool bIncludeEditorFeatures = false ) const;
	void applyTierQualityToState( t3dShaderVariantState_t &rState,
			const t3dTierQualityConfig_t &rQuality,
			const t3dShaderTier_t tier ) const;
	void applyPerformancePresetToTiers( const t3dShaderPerformancePreset_t preset );
	void tierQualityChanged( const t3dShaderTier_t tier, const bool bRecompile = true );
	void refreshGpuRegionChannelPolicy( void );
	void updateTierUniforms( const t3dShaderTier_t tier );
	String getTierRangeContext( const t3dShaderTier_t tier ) const;
	String getTierShadowContext( const t3dShaderTier_t tier ) const;
	String getTierShaderStatus( const t3dShaderTier_t tier ) const;
	t3dShaderVariantState_t makeTierState( const t3dShaderVariantState_t &rState, const t3dShaderTier_t tier ) const;
	void setRuntimeMaterialParam( const StringName &rName, const Variant &rValue );
	String generateBufferShaderCode( void ) const;
	String generateBufferShaderCode( const t3dShaderVariantState_t &rState ) const;
	String finalizeRuntimeShaderCode( const String &rShader ) const;
	String stripComments( const String &rShader ) const;
	String injectEditorCode( const String &rShader ) const;
	void updateShader( void );
	void refreshShaderPropertyCache( void );
	void disconnectShaderOverrideSignals( void );
	void connectShaderOverrideSignals( void );
	void ensureWorldNoiseCache( void );

	void invalidateRegionPageTables( void );
	bool updateRegionMapTexture( const PackedInt32Array &rRegionMap, int nRegionMapSize );
	bool updateLogicalRegionMapTexture( const PackedInt32Array &rRegionMap, int nRegionMapSize );
	t3dPageTableBuildResult_t prepareRegionPageTables( void );
	void bindRegionPageTableUniforms( const RID &rMaterial );
	void updateFarAtlasUniforms( const RID &rMaterial );
	void ensureShadowMaterials( void );
	void updateShadowUniforms( void );
	void updateUniforms( const RID &rMaterial, const uint32_t nUpdate = UNIFORMS_ONLY );
	void setShaderParameters( const Dictionary &rDict );
	EA_FORCE_INLINE Dictionary getShaderParameters( void ) const {
		return m_shaderParams;
	}

public:
	EA_FORCE_INLINE T3DMaterial( void ) {}
	EA_FORCE_INLINE ~T3DMaterial( void ) {
		destroy();
	}
	void initialize( T3DTerrain *pTerrain );
	EA_FORCE_INLINE bool isInitialized( void ) const {
		return m_pTerrain != nullptr;
	}
	EA_FORCE_INLINE bool isInitializedFor( const T3DTerrain *pTerrain ) const {
		return m_pTerrain == pTerrain;
	}
	EA_FORCE_INLINE T3DTerrain *getTerrainOwner( void ) const {
		return m_pTerrain;
	}
	void uninitialize( T3DTerrain *pTerrain );
	void destroy( void );

	void update( const uint32_t nFlags = UNIFORMS_ONLY );
	void updateEditorOverlay( void );
	EA_FORCE_INLINE bool isRenderShaderReady( void ) const {
		return m_bNearShaderReady && m_shader.is_valid() && m_shader->get_rid().is_valid();
	}
	void updateRegionPageTable( void );
	EA_FORCE_INLINE bool isRegionPageTableValid( void ) const {
		return m_generatedRegionMap.getRid().is_valid() && m_generatedLogicalRegionMap.getRid().is_valid() &&
				m_nRegionMapTextureSize > 0 && m_nLogicalRegionMapTextureSize == m_nRegionMapTextureSize;
	}
	void updateFarAtlasTextures( void );
	void updateRuntimeMaterialLayerMap( void );
	EA_FORCE_INLINE RID getMaterialRid( void ) const {
		return m_hMaterial;
	}
	RID getMaterialRidForTier( const t3dShaderTier_t tier ) const {
		if ( tier <= SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT || !m_hszTierMaterials[tier].is_valid() ) {
			return m_hMaterial;
		}
		return m_hszTierMaterials[tier];
	}
	RID getShadowMaterialRidForTier( const t3dShaderTier_t tier ) const {
		if ( tier < SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT || !m_hszShadowMaterials[tier].is_valid() ) {
			return getMaterialRidForTier( tier );
		}
		return m_hszShadowMaterials[tier];
	}
	RID getShaderRidForTier( const t3dShaderTier_t tier ) const {
		if ( tier <= SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT || m_szTierShaders[tier].is_null() ) {
			return getShaderRid();
		}
		return m_szTierShaders[tier]->get_rid();
	}
	EA_FORCE_INLINE RID getShaderRid( void ) const {
		return m_shader.is_valid() ? m_shader->get_rid() : RID();
	}

	EA_FORCE_INLINE RID getBufferMaterialRid( void ) const {
		return m_hBufferMaterial;
	}
	EA_FORCE_INLINE RID getBufferShaderRid( void ) const {
		return m_bufferShader.is_valid() ? m_bufferShader->get_rid() : RID();
	}

	// Material settings
	void setDisplacementScale( const real_t fDisplacementScale );
	EA_FORCE_INLINE real_t getDisplacementScale( void ) const {
		return m_fDisplacementScale;
	}
	void setDisplacementSharpness( const real_t fDisplacementSharpness );
	EA_FORCE_INLINE real_t getDisplacementSharpness( void ) const {
		return m_fDisplacementSharpness;
	}
	void setWorldBackground( const t3dWorldBackground_t background );
	EA_FORCE_INLINE t3dWorldBackground_t getWorldBackground( void ) const {
		return m_worldBackground;
	}
	// Shared 11-row quality-tier schema.
	void setNearTextureFiltering( t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getNearTextureFiltering( void ) const {
		return m_nearQuality.m_textureFiltering;
	}
	void setNearMaterialSamplingQuality( t3dMaterialSamplingQuality_t eQuality );
	EA_FORCE_INLINE t3dMaterialSamplingQuality_t getNearMaterialSamplingQuality( void ) const {
		return m_nearQuality.m_materialSamplingQuality;
	}
	void setNearTerrainNormalQuality( t3dTerrainNormalQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainNormalQuality_t getNearTerrainNormalQuality( void ) const {
		return m_nearQuality.m_terrainNormalQuality;
	}
	void setNearTerrainColorQuality( t3dTerrainColorQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainColorQuality_t getNearTerrainColorQuality( void ) const {
		return m_nearQuality.m_terrainColorQuality;
	}
	void setNearSurfaceResponseQuality( t3dSurfaceResponseQuality_t eQuality );
	EA_FORCE_INLINE t3dSurfaceResponseQuality_t getNearSurfaceResponseQuality( void ) const {
		return m_nearQuality.m_surfaceResponseQuality;
	}
	void setNearTextureSurfaceMode( t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getNearTextureSurfaceMode( void ) const {
		return m_nearQuality.m_textureSurfaceMode;
	}
	void setNearLightingModel( t3dTierLightingModel_t model );
	EA_FORCE_INLINE t3dTierLightingModel_t getNearLightingModel( void ) const {
		return m_nearQuality.m_lightingModel;
	}
	void setNearDetilingQuality( t3dTierDetilingQuality_t eQuality );
	EA_FORCE_INLINE t3dTierDetilingQuality_t getNearDetilingQuality( void ) const {
		return m_nearQuality.m_detilingQuality;
	}
	void setNearAuthoredColorContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getNearAuthoredColorContribution( void ) const {
		return m_nearQuality.m_fAuthoredColorContribution;
	}
	void setNearWetnessContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getNearWetnessContribution( void ) const {
		return m_nearQuality.m_fWetnessContribution;
	}
	void setNearMaterialLodBias( real_t fBias );
	EA_FORCE_INLINE real_t getNearMaterialLodBias( void ) const {
		return m_nearQuality.m_fMaterialLodBias;
	}

	void setMiddleMaterialSamplingQuality( t3dMaterialSamplingQuality_t eQuality );
	EA_FORCE_INLINE t3dMaterialSamplingQuality_t getMiddleMaterialSamplingQuality( void ) const {
		return m_middleQuality.m_materialSamplingQuality;
	}
	EA_FORCE_INLINE void setMiddleSurfaceSource( t3dMiddleSurfaceSource_t eSource ) {
		setMiddleMaterialSamplingQuality(
				eSource == MIDDLE_SURFACE_BAKED_COMPOSITE ? MATERIAL_SAMPLING_FAST : MATERIAL_SAMPLING_FULL );
	}
	EA_FORCE_INLINE t3dMiddleSurfaceSource_t getMiddleSurfaceSource( void ) const {
		return m_middleQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST ? MIDDLE_SURFACE_BAKED_COMPOSITE
																				   : MIDDLE_SURFACE_RUNTIME_MATERIAL;
	}
	void setMiddleTerrainNormalQuality( t3dTerrainNormalQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainNormalQuality_t getMiddleTerrainNormalQuality( void ) const {
		return m_middleQuality.m_terrainNormalQuality;
	}
	void setMiddleTerrainColorQuality( t3dTerrainColorQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainColorQuality_t getMiddleTerrainColorQuality( void ) const {
		return m_middleQuality.m_terrainColorQuality;
	}
	void setMiddleSurfaceResponseQuality( t3dSurfaceResponseQuality_t eQuality );
	EA_FORCE_INLINE t3dSurfaceResponseQuality_t getMiddleSurfaceResponseQuality( void ) const {
		return m_middleQuality.m_surfaceResponseQuality;
	}
	void setMiddleLightingModel( t3dTierLightingModel_t model );
	EA_FORCE_INLINE t3dTierLightingModel_t getMiddleLightingModel( void ) const {
		return m_middleQuality.m_lightingModel;
	}
	void setMiddleDetilingQuality( t3dTierDetilingQuality_t eQuality );
	EA_FORCE_INLINE t3dTierDetilingQuality_t getMiddleDetilingQuality( void ) const {
		return m_middleQuality.m_detilingQuality;
	}
	void setMiddleAuthoredColorContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getMiddleAuthoredColorContribution( void ) const {
		return m_middleQuality.m_fAuthoredColorContribution;
	}
	void setMiddleWetnessContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getMiddleWetnessContribution( void ) const {
		return m_middleQuality.m_fWetnessContribution;
	}

	void setFarMaterialSamplingQuality( t3dMaterialSamplingQuality_t eQuality );
	EA_FORCE_INLINE t3dMaterialSamplingQuality_t getFarMaterialSamplingQuality( void ) const {
		return m_farQuality.m_materialSamplingQuality;
	}
	void setFarTerrainNormalQuality( t3dTerrainNormalQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainNormalQuality_t getFarTerrainNormalQuality( void ) const {
		return m_farQuality.m_terrainNormalQuality;
	}
	void setFarTerrainColorQuality( t3dTerrainColorQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainColorQuality_t getFarTerrainColorQuality( void ) const {
		return m_farQuality.m_terrainColorQuality;
	}
	void setFarSurfaceResponseQuality( t3dSurfaceResponseQuality_t eQuality );
	EA_FORCE_INLINE t3dSurfaceResponseQuality_t getFarSurfaceResponseQuality( void ) const {
		return m_farQuality.m_surfaceResponseQuality;
	}
	void setFarLightingModel( t3dTierLightingModel_t model );
	EA_FORCE_INLINE t3dTierLightingModel_t getFarLightingModel( void ) const {
		return m_farQuality.m_lightingModel;
	}
	void setFarDetilingQuality( t3dTierDetilingQuality_t eQuality );
	EA_FORCE_INLINE t3dTierDetilingQuality_t getFarDetilingQuality( void ) const {
		return m_farQuality.m_detilingQuality;
	}
	void setFarAuthoredColorContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getFarAuthoredColorContribution( void ) const {
		return m_farQuality.m_fAuthoredColorContribution;
	}
	void setFarWetnessContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getFarWetnessContribution( void ) const {
		return m_farQuality.m_fWetnessContribution;
	}
	void setFarMaterialLodBias( real_t fBias );
	EA_FORCE_INLINE real_t getFarMaterialLodBias( void ) const {
		return m_farQuality.m_fMaterialLodBias;
	}

	EA_FORCE_INLINE String getNearRangeContext( void ) const {
		return getTierRangeContext( SHADER_TIER_NEAR );
	}
	EA_FORCE_INLINE String getMiddleRangeContext( void ) const {
		return getTierRangeContext( SHADER_TIER_MIDDLE );
	}
	EA_FORCE_INLINE String getFarRangeContext( void ) const {
		return getTierRangeContext( SHADER_TIER_FAR );
	}
	EA_FORCE_INLINE String getNearShadowContext( void ) const {
		return getTierShadowContext( SHADER_TIER_NEAR );
	}
	EA_FORCE_INLINE String getMiddleShadowContext( void ) const {
		return getTierShadowContext( SHADER_TIER_MIDDLE );
	}
	EA_FORCE_INLINE String getFarShadowContext( void ) const {
		return getTierShadowContext( SHADER_TIER_FAR );
	}
	EA_FORCE_INLINE String getNearShaderStatus( void ) const {
		return getTierShaderStatus( SHADER_TIER_NEAR );
	}
	EA_FORCE_INLINE String getMiddleShaderStatus( void ) const {
		return getTierShaderStatus( SHADER_TIER_MIDDLE );
	}
	EA_FORCE_INLINE String getFarShaderStatus( void ) const {
		return getTierShaderStatus( SHADER_TIER_FAR );
	}

	// Hidden serialized compatibility aliases. Runtime state is owned only by the
	// canonical t3dTierQualityConfig_t rows above.
	void setTextureFiltering( const t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getTextureFiltering( void ) const {
		return m_nearQuality.m_textureFiltering;
	}
	void setShaderVariantMode( const t3dShaderVariantMode_t mode );
	EA_FORCE_INLINE t3dShaderVariantMode_t getShaderVariantMode( void ) const {
		return m_shaderVariantMode;
	}
	void setShaderVariantCacheLimit( const int nLimit );
	EA_FORCE_INLINE int getShaderVariantCacheLimit( void ) const {
		return m_nShaderVariantCacheLimit;
	}
	void setShaderVariantPrewarmLimit( const int nLimit );
	EA_FORCE_INLINE int getShaderVariantPrewarmLimit( void ) const {
		return m_nShaderVariantPrewarmLimit;
	}
	void setShaderVariantSourceBudgetMb( const int nBudgetMb );
	EA_FORCE_INLINE int getShaderVariantSourceBudgetMb( void ) const {
		return m_nShaderVariantSourceBudgetMb;
	}
	void setEffectiveShaderVariantSourceBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getEffectiveShaderVariantSourceBudgetMb( void ) const {
		return m_nShaderVariantEffectiveSourceBudgetMb;
	}
	EA_FORCE_INLINE size_t getShaderVariantSourceBytesNative( void ) const {
		return getShaderVariantSourceBytesRaw();
	}
	void setShaderPerformancePreset( const t3dShaderPerformancePreset_t preset );
	EA_FORCE_INLINE t3dShaderPerformancePreset_t getShaderPerformancePreset( void ) const {
		return m_shaderPerformancePreset;
	}
	// Middle tier quality. Existing unprefixed quality properties remain the Near tier.
	void setMiddleTextureFiltering( t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getMiddleTextureFiltering( void ) const {
		return m_middleQuality.m_textureFiltering;
	}
	void setMiddleTerrainSamplingMode( t3dTerrainSamplingMode_t mode );
	t3dTerrainSamplingMode_t getMiddleTerrainSamplingMode( void ) const;
	void setMiddleTerrainNormalMode( t3dTerrainNormalMode_t mode );
	t3dTerrainNormalMode_t getMiddleTerrainNormalMode( void ) const;
	void setMiddleColorSamplingMode( t3dColorSamplingMode_t mode );
	t3dColorSamplingMode_t getMiddleColorSamplingMode( void ) const;
	void setMiddleSurfaceDataMode( t3dSurfaceDataMode_t mode );
	t3dSurfaceDataMode_t getMiddleSurfaceDataMode( void ) const;
	void setMiddleTextureSurfaceMode( t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getMiddleTextureSurfaceMode( void ) const {
		return m_middleQuality.m_textureSurfaceMode;
	}
	void setMiddleTerrainShadingMode( t3dTerrainShadingMode_t mode );
	t3dTerrainShadingMode_t getMiddleTerrainShadingMode( void ) const;
	void setMiddleDetilingMode( t3dDetilingMode_t mode );
	t3dDetilingMode_t getMiddleDetilingMode( void ) const;
	void setMiddleAutoShaderEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getMiddleAutoShaderEnabled( void ) const {
		return m_bAutoShaderEnabled;
	}
	void setMiddleAuthoredColorEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getMiddleAuthoredColorEnabled( void ) const {
		return m_middleQuality.m_fAuthoredColorContribution > 0.0f;
	}
	void setMiddleWetnessEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getMiddleWetnessEnabled( void ) const {
		return m_middleQuality.m_fWetnessContribution > 0.0f;
	}
	void setMiddleMaterialLodBias( real_t fBias );
	EA_FORCE_INLINE real_t getMiddleMaterialLodBias( void ) const {
		return m_middleQuality.m_fMaterialLodBias;
	}

	// Far tier quality. The far shader intentionally remains a dominant-material, one-albedo-sample path.
	void setFarTextureFiltering( t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getFarTextureFiltering( void ) const {
		return m_farQuality.m_textureFiltering;
	}
	void setFarTextureSurfaceMode( t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getFarTextureSurfaceMode( void ) const {
		return m_farQuality.m_textureSurfaceMode;
	}
	void setFarTerrainShadingMode( t3dTerrainShadingMode_t mode );
	t3dTerrainShadingMode_t getFarTerrainShadingMode( void ) const;
	void setFarMaterialLod( real_t fLod );
	EA_FORCE_INLINE real_t getFarMaterialLod( void ) const {
		return m_farQuality.m_fMaterialLodBias + 4.0f;
	}
	void setFarAuthoredColorEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getFarAuthoredColorEnabled( void ) const {
		return m_farQuality.m_fAuthoredColorContribution > 0.0f;
	}
	void setFarWetnessEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getFarWetnessEnabled( void ) const {
		return m_farQuality.m_fWetnessContribution > 0.0f;
	}

	void setTerrainSamplingMode( const t3dTerrainSamplingMode_t mode );
	EA_FORCE_INLINE t3dTerrainSamplingMode_t getTerrainSamplingMode( void ) const {
		return m_nearQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST ? TERRAIN_SAMPLING_SINGLE_SAMPLE
																				 : TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED;
	}
	void setTerrainNormalMode( const t3dTerrainNormalMode_t mode );
	EA_FORCE_INLINE t3dTerrainNormalMode_t getTerrainNormalMode( void ) const {
		return m_nearQuality.m_terrainNormalQuality == TERRAIN_NORMAL_QUALITY_GEOMETRY
				? TERRAIN_NORMAL_GEOMETRY_DERIVATIVE
				: TERRAIN_NORMAL_DERIVED_TEXTURE;
	}
	void setColorSamplingMode( const t3dColorSamplingMode_t mode );
	EA_FORCE_INLINE t3dColorSamplingMode_t getColorSamplingMode( void ) const {
		return m_nearQuality.m_terrainColorQuality == TERRAIN_COLOR_QUALITY_FAST ? COLOR_SAMPLING_HARDWARE_FILTERED
																				 : COLOR_SAMPLING_EXACT_4_TAP;
	}
	void setProjectionMode( const t3dProjectionMode_t mode );
	EA_FORCE_INLINE t3dProjectionMode_t getProjectionMode( void ) const {
		return m_projectionMode;
	}
	void setTessellationMaterialMode( const t3dTessellationMaterialMode_t mode );
	EA_FORCE_INLINE t3dTessellationMaterialMode_t getTessellationMaterialMode( void ) const {
		return m_tessellationMaterialMode;
	}
	void setSurfaceDataMode( const t3dSurfaceDataMode_t mode );
	EA_FORCE_INLINE t3dSurfaceDataMode_t getSurfaceDataMode( void ) const {
		return m_nearQuality.m_surfaceResponseQuality == SURFACE_RESPONSE_SIMPLE ? SURFACE_DATA_ALBEDO_ONLY
																				 : SURFACE_DATA_FULL;
	}
	void setTextureSurfaceMode( const t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getTextureSurfaceMode( void ) const {
		return m_nearQuality.m_textureSurfaceMode;
	}
	void setTerrainShadingMode( const t3dTerrainShadingMode_t mode );
	t3dTerrainShadingMode_t getTerrainShadingMode( void ) const {
		return m_nearQuality.m_lightingModel == TIER_LIGHTING_UNSHADED
				? SHADING_UNSHADED
				: ( m_nearQuality.m_lightingModel == TIER_LIGHTING_BLINN_PHONG
								  ? SHADING_XBOX_360_BLINN_PHONG
								  : ( m_nearQuality.m_lightingModel == TIER_LIGHTING_PBR
													? SHADING_BURLEY_GGX
													: SHADING_LAMBERT_DIFFUSE_ONLY ) );
	}
	void setDetilingMode( const t3dDetilingMode_t mode );
	EA_FORCE_INLINE t3dDetilingMode_t getDetilingMode( void ) const {
		return m_nearQuality.m_detilingQuality == TIER_DETILING_DISABLED ? DETILING_DISABLED : DETILING_DISCRETE_8;
	}
	void setWorldNoiseShaderMode( const t3dWorldNoiseShaderMode_t mode );
	EA_FORCE_INLINE t3dWorldNoiseShaderMode_t getWorldNoiseShaderMode( void ) const {
		return m_worldNoiseShaderMode;
	}
	void prewarmShaderVariants( void );
	Dictionary cookRuntimeShaderPack( void );
	bool loadRuntimeShaderPack( void );
	EA_FORCE_INLINE bool hasRuntimeShaderPack( void ) const {
		return m_bRuntimeShaderPackLoaded;
	}
	void clearShaderVariantCache( void );
	EA_FORCE_INLINE int getShaderVariantCount( void ) const {
		return m_nRuntimeShaderVariantCount + m_nEditorShaderVariantCount;
	}
	EA_FORCE_INLINE int getRuntimeShaderVariantCount( void ) const {
		return m_nRuntimeShaderVariantCount;
	}
	EA_FORCE_INLINE int getEditorShaderVariantCount( void ) const {
		return m_nEditorShaderVariantCount;
	}
	EA_FORCE_INLINE int getBufferShaderVariantCount( void ) const {
		return m_nBufferShaderVariantCount;
	}
	int getSpeculativeShaderVariantCount( void ) const;
	EA_FORCE_INLINE int64_t getShaderVariantSourceBytes( void ) const {
		return ( int64_t )( getShaderVariantSourceBytesRaw() );
	}
	EA_FORCE_INLINE int getLastPrewarmVariantCount( void ) const {
		return m_nLastPrewarmVariantCount;
	}
	EA_FORCE_INLINE int getLastPrewarmSkippedCount( void ) const {
		return m_nLastPrewarmSkippedCount;
	}
	EA_FORCE_INLINE int64_t getTheoreticalShaderVariantCount( void ) const {
		return 1492992000LL;
	}
	Dictionary getShaderVariantDiagnostics( void ) const;
	Dictionary getShaderTierDiagnostics( const int nTier ) const;
	void resetShaderVariantDiagnostics( void );
	String getShaderRegressionSource( const int nTier,
			const int nAssemblyPath,
			const bool bControlMapsImplicit,
			const bool bColorMapsImplicit );
#ifdef DEBUG_ENABLED
	void setShaderRegressionInvalidTier( const int nTier );
	Variant getTierRegressionMaterialParam( const int nTier, const StringName &rName ) const;
#endif
	EA_FORCE_INLINE uint64_t getShaderUpdateCount( void ) const {
		return m_nShaderUpdateCount;
	}
	EA_FORCE_INLINE uint64_t getShaderVariantCreationCount( void ) const {
		return m_nVariantCreationsTotal;
	}
	EA_FORCE_INLINE uint64_t getShaderSourceSubmissionUsecLast( void ) const {
		return m_nVariantSourceSubmissionUsecLast;
	}
	EA_FORCE_INLINE uint64_t getShaderUpdateUsecLast( void ) const {
		return m_nShaderUpdateUsecLast;
	}
	void setPackedPbrSingleFetch( const bool bEnabled );
	EA_FORCE_INLINE bool getPackedPbrSingleFetch( void ) const {
		return false;
	}
	void setAutoShaderEnabled( const bool bEnabled );
	// Editor preview helper. This changes only the active shader state and does
	// not alter the serialized performance preset or dirty the material resource.
	void setAutoShaderEnabledTransient( const bool bEnabled );
	EA_FORCE_INLINE bool getAutoShaderEnabled( void ) const {
		return m_bAutoShaderEnabled;
	}
	void setDualScalingEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getDualScalingEnabled( void ) const {
		return m_bDualScalingEnabled;
	}
	void setMacroVariationEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getMacroVariationEnabled( void ) const {
		return m_bMacroVariationEnabled;
	}
	void setProjectionEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getProjectionEnabled( void ) const {
		return m_projectionMode != PROJECTION_DISABLED;
	}

	void setShaderOverrideEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool isShaderOverrideEnabled( void ) const {
		return m_bShaderOverrideEnabled;
	}
	void setShaderOverride( const Ref<Shader> &rShader );
	EA_FORCE_INLINE Ref<Shader> getShaderOverride( void ) const {
		return m_shaderOverride;
	}

	void setBufferShaderOverrideEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool isBufferShaderOverrideEnabled( void ) const {
		return m_bBufferShaderOverrideEnabled;
	}
	void setBufferShaderOverride( const Ref<Shader> &rShader );
	EA_FORCE_INLINE Ref<Shader> getBufferShaderOverride( void ) const {
		return m_bufferShaderOverride;
	}

	void setShaderParam( const StringName &rName, const Variant &rValue );
	Variant getShaderParam( const StringName &rName ) const;

	// PBR outputs
	void setOutputAlbedoEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputAlbedoEnabled( void ) const {
		return m_bOutputAlbedoEnabled;
	}
	void setOutputRoughnessEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputRoughnessEnabled( void ) const {
		return m_bOutputRoughnessEnabled;
	}
	void setOutputSpecularEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputSpecularEnabled( void ) const {
		return m_bOutputSpecularEnabled;
	}
	void setOutputNormalMapEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputNormalMapEnabled( void ) const {
		return m_bOutputNormalMapEnabled;
	}
	void setOutputAmbientOcclusionEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputAmbientOcclusionEnabled( void ) const {
		return m_bOutputAmbientOcclusionEnabled;
	}

	// Overlays
	void setShowRegionGrid( const bool bEnabled );
	EA_FORCE_INLINE bool getShowRegionGrid( void ) const {
		return m_bShowRegionGrid;
	}
	void setShowInstancerGrid( const bool bEnabled );
	EA_FORCE_INLINE bool getShowInstancerGrid( void ) const {
		return m_bShowInstancerGrid;
	}
	void setShowVertexGrid( const bool bEnabled );
	EA_FORCE_INLINE bool getShowVertexGrid( void ) const {
		return m_bShowVertexGrid;
	}
	void setShowContours( const bool bEnabled );
	EA_FORCE_INLINE bool getShowContours( void ) const {
		return m_bShowContours;
	}
	void setShowNavigation( const bool bEnabled );
	EA_FORCE_INLINE bool getShowNavigation( void ) const {
		return m_bShowNavigation;
	}

	// Debug views
	void setShowCheckered( const bool bEnabled );
	EA_FORCE_INLINE bool getShowCheckered( void ) const {
		return m_bDebugViewCheckered;
	}
	void setShowGrey( const bool bEnabled );
	EA_FORCE_INLINE bool getShowGrey( void ) const {
		return m_bDebugViewGrey;
	}
	void setShowHeightmap( const bool bEnabled );
	EA_FORCE_INLINE bool getShowHeightmap( void ) const {
		return m_bDebugViewHeightmap;
	}
	void setShowJaggedness( const bool bEnabled );
	EA_FORCE_INLINE bool getShowJaggedness( void ) const {
		return m_bDebugViewJaggedness;
	}
	void setShowAutoshader( const bool bEnabled );
	EA_FORCE_INLINE bool getShowAutoshader( void ) const {
		return m_bDebugViewAutoshader;
	}
	void setShowControlTexture( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlTexture( void ) const {
		return m_bDebugViewControlTexture;
	}
	void setShowControlBlend( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlBlend( void ) const {
		return m_bDebugViewControlBlend;
	}
	void setShowControlAngle( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlAngle( void ) const {
		return m_bDebugViewControlAngle;
	}
	void setShowControlScale( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlScale( void ) const {
		return m_bDebugViewControlScale;
	}
	void setShowColormap( const bool bEnabled );
	EA_FORCE_INLINE bool getShowColormap( void ) const {
		return m_bDebugViewColormap;
	}
	void setShowRoughmap( const bool bEnabled );
	EA_FORCE_INLINE bool getShowRoughmap( void ) const {
		return m_bDebugViewRoughmap;
	}
	void setShowDisplacementBuffer( const bool bEnabled );
	EA_FORCE_INLINE bool getShowDisplacementBuffer( void ) const {
		return m_bDebugViewDisplacementBuffer;
	}

	// PBR Views
	void setShowTextureAlbedo( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureAlbedo( void ) const {
		return m_bPbrViewTexAlbedo;
	}
	void setShowTextureHeight( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureHeight( void ) const {
		return m_bPbrViewTexHeight;
	}
	void setShowTextureNormal( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureNormal( void ) const {
		return m_bPbrViewTexNormal;
	}
	void setShowTextureRough( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureRough( void ) const {
		return m_bPbrViewTexRough;
	}
	void setShowTextureAo( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureAo( void ) const {
		return m_bPbrViewTexAo;
	}

	Error save( const String &rPath = "" );

protected:
	void _get_property_list( List<PropertyInfo> *pList ) const;
	bool _property_can_revert( const StringName &rName ) const;
	bool _property_get_revert( const StringName &rName, Variant &rProperty ) const;
	bool _set( const StringName &rName, const Variant &rProperty );
	bool _get( const StringName &rName, Variant &rProperty ) const;

	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dWorldBackground_t );
VARIANT_ENUM_CAST( t3dTextureFiltering_t );
VARIANT_ENUM_CAST( t3dUpdateFlags_t );
VARIANT_ENUM_CAST( t3dShaderVariantMode_t );
VARIANT_ENUM_CAST( t3dShaderPerformancePreset_t );
VARIANT_ENUM_CAST( t3dTerrainSamplingMode_t );
VARIANT_ENUM_CAST( t3dTerrainNormalMode_t );
VARIANT_ENUM_CAST( t3dColorSamplingMode_t );
VARIANT_ENUM_CAST( t3dProjectionMode_t );
VARIANT_ENUM_CAST( t3dTessellationMaterialMode_t );
VARIANT_ENUM_CAST( t3dSurfaceDataMode_t );
VARIANT_ENUM_CAST( t3dTextureSurfaceMode_t );
VARIANT_ENUM_CAST( t3dTerrainShadingMode_t );
VARIANT_ENUM_CAST( t3dDetilingMode_t );
VARIANT_ENUM_CAST( t3dMaterialSamplingQuality_t );
VARIANT_ENUM_CAST( t3dMiddleSurfaceSource_t );
VARIANT_ENUM_CAST( t3dTerrainNormalQuality_t );
VARIANT_ENUM_CAST( t3dTerrainColorQuality_t );
VARIANT_ENUM_CAST( t3dSurfaceResponseQuality_t );
VARIANT_ENUM_CAST( t3dTierLightingModel_t );
VARIANT_ENUM_CAST( t3dTierDetilingQuality_t );
VARIANT_ENUM_CAST( t3dWorldNoiseShaderMode_t );
#endif // TERRAIN3D_MATERIAL_CLASS_H
