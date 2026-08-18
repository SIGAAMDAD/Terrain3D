// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MATERIAL_CLASS_H
#define TERRAIN3D_MATERIAL_CLASS_H

#include <cstdint>
#include "eastl_config.h"
#include <EASTL/vector.h>

#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/noise_texture2d.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"

class T3DTerrain;


class T3DMaterial : public Resource {
	using Terrain3DMaterial = T3DMaterial;
	GDCLASS( Terrain3DMaterial, Resource );
	CLASS_NAME();

	bool m_bPersistenceDirty = false;

public: // Constants
	typedef enum t3dworldBackground_e {
		NONE,
		FLAT,
		NOISE,
	} t3dworldBackground_t;

	typedef enum t3dtextureFiltering_e {
		LINEAR_ANISOTROPIC,
		LINEAR,
		NEAREST_ANISOTROPIC,
		NEAREST,
	} t3dtextureFiltering_t;

	typedef enum t3dupdateFlags_e {
		UNIFORMS_ONLY = 0,
		TEXTURE_ARRAYS = 1 << 0,
		REGION_ARRAYS = 1 << 1,
		UPDATE_ARRAYS = TEXTURE_ARRAYS | REGION_ARRAYS,
		FULL_REBUILD = ( 1 << 2 ) | UPDATE_ARRAYS,
	} t3dupdateFlags_t;

	// Controls the compile-time shader permutation cache. Feature modules
	// are gated by preprocessor defines and resolved through standard shader includes.
	typedef enum t3dshaderVariantMode_e {
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
	} t3dshaderVariantMode_t;

	// High-level profiles exposed by the Terrain3D editor sidebar. CUSTOM keeps
	// every individual hotpath control available. The other profiles assign a
	// bounded group of compile-time modes and then compile/cache that exact path.
	typedef enum t3dshaderPerformancePreset_e {
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
	} t3dshaderPerformancePreset_t;

	// Independently compiled clipmap shader tiers. These are internal runtime
	// materials, not user-facing material resources. Near preserves the selected
	// surface model, middle resolves only control/color/derived-normal data, and
	// far uses the coarse world atlases.
	typedef enum t3dshaderTier_e {
		SHADER_TIER_NEAR,
		SHADER_TIER_MIDDLE,
		SHADER_TIER_FAR,
		SHADER_TIER_COUNT,
	} t3dshaderTier_t;

	// Controls terrain control-map and normal interpolation. Static modes remove
	// the distance-dependent bilinear branch from the generated fragment shader.
	typedef enum t3dterrainSamplingMode_e {
		TERRAIN_SAMPLING_ADAPTIVE,
		TERRAIN_SAMPLING_ALWAYS_SMOOTH,
		TERRAIN_SAMPLING_SINGLE_SAMPLE,
		// Appended so existing serialized values remain stable.
		TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED,
		TERRAIN_SAMPLING_DISTANCE_ADAPTIVE,
		TERRAIN_SAMPLING_ADAPTIVE_2_CORNER,
	} t3dterrainSamplingMode_t;

	// Controls the final terrain geometric normal source. Static modes remove the
	// public flat_terrain_normals uniform branch.
	typedef enum t3dterrainNormalMode_e {
		TERRAIN_NORMAL_ADAPTIVE,
		TERRAIN_NORMAL_HEIGHT_8_TAP,
		TERRAIN_NORMAL_GEOMETRY_DERIVATIVE,
		TERRAIN_NORMAL_DERIVED_TEXTURE,
		TERRAIN_NORMAL_HEIGHT_3_TAP,
		// Serialized compatibility aliases.
		TERRAIN_NORMAL_SMOOTH = TERRAIN_NORMAL_HEIGHT_8_TAP,
		TERRAIN_NORMAL_FLAT = TERRAIN_NORMAL_GEOMETRY_DERIVATIVE,
	} t3dterrainNormalMode_t;

	// Controls regional color/wetness filtering. AUTO uses one hardware-filtered
	// sample inside a region and exact four-corner sampling only at layer edges.
	typedef enum t3dcolorSamplingMode_e {
		COLOR_SAMPLING_AUTO,
		COLOR_SAMPLING_HARDWARE_FILTERED,
		COLOR_SAMPLING_EXACT_4_TAP,
	} t3dcolorSamplingMode_t;

	// Controls steep-surface UV projection. SHARED uses the final fragment terrain
	// normal for all material corners; EXACT evaluates each corner independently.
	typedef enum t3dprojectionMode_e {
		PROJECTION_DISABLED,
		PROJECTION_SHARED_FRAGMENT,
		PROJECTION_EXACT_PER_CORNER,
	} t3dprojectionMode_t;

	// Controls whether tessellation displacement is sampled again in the fragment
	// material path for height blending/AO suppression.
	typedef enum t3dtessellationMaterialMode_e {
		TESSELLATION_GEOMETRY_ONLY,
		TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT,
	} t3dtessellationMaterialMode_t;

	// Controls whether the material surface array is fetched. AUTO retains the
	// fetch only when an enabled PBR output consumes normal/roughness/AO data.
	typedef enum t3dsurfaceDataMode_e {
		SURFACE_DATA_AUTO,
		SURFACE_DATA_FULL,
		SURFACE_DATA_ALBEDO_ONLY,
	} t3dsurfaceDataMode_t;

	// Controls whether terrain texture arrays participate in the surface pass.
	// SILHOUETTE preserves all authored texture/control data while compiling the
	// material texture sampling path out of the terrain shaders.
	typedef enum t3dtextureSurfaceMode_e {
		TEXTURE_SURFACE_TEXTURED,
		TEXTURE_SURFACE_SILHOUETTE,
	} t3dtextureSurfaceMode_t;

	// Selects compile-time Godot spatial render modes and lighting frequency.
	typedef enum t3dterrainShadingMode_e {
		SHADING_BURLEY_GGX,
		SHADING_LAMBERT_GGX,
		SHADING_LAMBERT_DIFFUSE_ONLY,
		SHADING_UNSHADED,
		// Appended to preserve serialized values of the original four modes.
		SHADING_LAMBERT_VERTEX,
		// Non-PBR Lambert + Blinn-Phong response approximating common 2005-2013 terrain renderers.
		SHADING_XBOX_360_BLINN_PHONG,
	} t3dterrainShadingMode_t;

	// PATCH-02: one semantic quality schema shared by Near, Middle, and Far.
	// Each tier maps these choices onto a tier-appropriate implementation rather
	// than exposing the legacy Near shader internals directly.
	typedef enum t3dmaterialSamplingQuality_e {
		MATERIAL_SAMPLING_FAST,
		MATERIAL_SAMPLING_FULL,
	} t3dmaterialSamplingQuality_t;

	typedef enum t3dterrainNormalQuality_e {
		TERRAIN_NORMAL_QUALITY_GEOMETRY,
		TERRAIN_NORMAL_QUALITY_DETAILED,
	} t3dterrainNormalQuality_t;

	typedef enum t3dterrainColorQuality_e {
		TERRAIN_COLOR_QUALITY_FAST,
		TERRAIN_COLOR_QUALITY_EXACT,
	} t3dterrainColorQuality_t;

	typedef enum t3dsurfaceResponseQuality_e {
		SURFACE_RESPONSE_SIMPLE,
		SURFACE_RESPONSE_DETAILED,
	} t3dsurfaceResponseQuality_t;

	typedef enum t3dtierLightingModel_e {
		TIER_LIGHTING_UNSHADED,
		TIER_LIGHTING_LAMBERT,
		TIER_LIGHTING_BLINN_PHONG,
		TIER_LIGHTING_PBR,
	} t3dtierLightingModel_t;

	typedef enum t3dtierDetilingQuality_e {
		TIER_DETILING_DISABLED,
		TIER_DETILING_ROTATION,
	} t3dtierDetilingQuality_t;

	// Detiling quality/cost. DISCRETE_8 replaces the sine hash plus per-material
	// sin/cos pair with an integer hash and an eight-entry rotation table.
	typedef enum t3ddetilingMode_e {
		DETILING_CONTINUOUS,
		DETILING_DISCRETE_8,
		DETILING_DISABLED,
		// Appended so existing serialized values remain stable.
		DETILING_TRANSLATION_ONLY,
		DETILING_DISCRETE_4,
		DETILING_DISCRETE_16,
	} t3ddetilingMode_t;

	// Dynamic preserves the public uniforms. The static modes remove both world
	// noise source branches from the compiled shader.
	typedef enum t3dworldNoiseShaderMode_e {
		WORLD_NOISE_DYNAMIC,
		WORLD_NOISE_CACHED_VERTEX_NORMALS,
		WORLD_NOISE_CACHED_FRAGMENT_NORMALS,
		WORLD_NOISE_PROCEDURAL_VERTEX_NORMALS,
		WORLD_NOISE_PROCEDURAL_FRAGMENT_NORMALS,
	} t3dworldNoiseShaderMode_t;

private:
typedef enum t3dshaderVariantDomain_e {
		SHADER_DOMAIN_RUNTIME,
		SHADER_DOMAIN_EDITOR,
		SHADER_DOMAIN_BUFFER,
	} t3dshaderVariantDomain_t;

	typedef enum t3dshaderVariantDimension_e {
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
	} t3dshaderVariantDimension_t;

	typedef struct t3dshaderVariantState_s {
		t3dworldBackground_t worldBackground = FLAT;
		t3dtextureFiltering_t textureFiltering = LINEAR;
		t3dterrainSamplingMode_t terrainSamplingMode = TERRAIN_SAMPLING_ADAPTIVE;
		t3dterrainNormalMode_t terrainNormalMode = TERRAIN_NORMAL_ADAPTIVE;
		t3dcolorSamplingMode_t colorSamplingMode = COLOR_SAMPLING_AUTO;
		t3dprojectionMode_t projectionMode = PROJECTION_DISABLED;
		t3dtessellationMaterialMode_t tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
		t3dsurfaceDataMode_t surfaceDataMode = SURFACE_DATA_AUTO;
		t3dtextureSurfaceMode_t textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
		t3dterrainShadingMode_t terrainShadingMode = SHADING_LAMBERT_GGX;
		t3ddetilingMode_t detilingMode = DETILING_CONTINUOUS;
		t3dworldNoiseShaderMode_t worldNoiseShaderMode = WORLD_NOISE_DYNAMIC;
		bool bAutoShaderEnabled = false;
		bool bDualScalingEnabled = false;
		bool bMacroVariationEnabled = false;
		bool bOutputAlbedoEnabled = true;
		bool bOutputRoughnessEnabled = true;
		bool bOutputSpecularEnabled = true;
		bool bOutputNormalMapEnabled = true;
		bool bOutputAmbientOcclusionEnabled = true;
		bool bFastBlendCurve = false;
		bool bTessellationEnabled = false;
		bool bSurfacePacked = false;
		bool bControlMapsImplicit = false;
		bool bColorMapsImplicit = false;
	} t3dshaderVariantState_t;

	typedef struct t3dtierQualityConfig_s {
		t3dtextureFiltering_t textureFiltering = LINEAR;
		t3dmaterialSamplingQuality_t materialSamplingQuality = MATERIAL_SAMPLING_FULL;
		t3dterrainNormalQuality_t terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
		t3dterrainColorQuality_t terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
		t3dsurfaceResponseQuality_t surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
		t3dtextureSurfaceMode_t textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
		t3dtierLightingModel_t lightingModel = TIER_LIGHTING_LAMBERT;
		t3dtierDetilingQuality_t detilingQuality = TIER_DETILING_DISABLED;
		real_t fAuthoredColorContribution = 1.0f;
		real_t fWetnessContribution = 1.0f;
		real_t fMaterialLodBias = 0.0f;
	} t3dtierQualityConfig_t;

	typedef struct t3dshaderVariantKey_s {
		uint64_t primaryHash = 0;
		uint64_t fingerprint = 0;
		uint32_t nSourceLength = 0;

		bool operator==( const t3dshaderVariantKey_s &rOther ) const {
			return primaryHash == rOther.primaryHash && fingerprint == rOther.fingerprint &&
				   nSourceLength == rOther.nSourceLength;
		}
	} t3dshaderVariantKey_t;

	typedef struct t3dshaderValidationResult_s {
		bool bValid = false;
		int nReflectedParameterCount = 0;
		uint64_t shaderRidId = 0u;
		String failureSummary;
	} t3dshaderValidationResult_t;

	typedef struct t3dshaderTierValidationState_s {
		t3dshaderVariantKey_t key;
		uint64_t requestedShaderRidId = 0u;
		uint64_t activeShaderRidId = 0u;
		bool bCompileValid = false;
		int nReflectedParameterCount = 0;
		String failureSummary;
		bool bFallbackToNear = false;
		bool bCompactTierActive = false;
	} t3dshaderTierValidationState_t;

	typedef struct t3dshaderVariantEntry_s {
		t3dshaderVariantKey_t key;
		Ref<Shader> pShader;
		uint32_t nSourceBytes = 0;
		int nReflectedParameterCount = 0;
		bool bSpeculative = true;
	} t3dshaderVariantEntry_t;

	T3DTerrain *m_pTerrain = nullptr;

	RID m_pMaterial;
	RID m_szTierMaterials[SHADER_TIER_COUNT] = {};
	Ref<Shader> m_pShader;	 // Active near shader
	bool m_bNearShaderReady = false;
	Ref<Shader> m_szTierShaders[SHADER_TIER_COUNT];
	Dictionary m_shaderTemplates; // External root shader templates loaded from the addon
	t3dshaderVariantEntry_t m_szRuntimeShaderVariantEntries[TERRAIN3D_MAX_SHADER_VARIANT_CACHE_LIMIT] = {};
	t3dshaderVariantEntry_t m_szEditorShaderVariantEntries[TERRAIN3D_MAX_EDITOR_SHADER_VARIANT_CACHE_LIMIT] = {};
	int m_nRuntimeShaderVariantCount = 0;
	int m_nEditorShaderVariantCount = 0;
	uint64_t m_nRuntimeShaderVariantSourceBytes = 0;
	uint64_t m_nEditorShaderVariantSourceBytes = 0;
	bool m_bShaderOverrideEnabled = false;
	Ref<Shader> m_pShaderOverride; // User's shader we copy code from
	Ref<Shader> m_pConnectedShaderOverride;
	mutable TypedArray<StringName> m_activeParams; // All public shader params in the current shader
	mutable Dictionary m_shaderParams;			   // Public shader params saved to disk
	eastl::vector<PropertyInfo> m_shaderPropertyCache;
	Dictionary m_shaderParamDefaults;
	// Mirrors values pushed to private/runtime uniforms so hot callers never read
	// RenderingServer material state back to the CPU.
	mutable Dictionary m_runtimeMaterialParams;
	uint64_t m_nShaderParameterListQueryCount = 0u;
	uint64_t m_nShaderParameterDefaultQueryCount = 0u;
	mutable uint64_t m_nShaderInspectorRuntimeReadCount = 0u;

	RID m_bufferMaterial;
	Ref<Shader> m_pBufferShader; // Active buffer shader
	t3dshaderVariantEntry_t m_szBufferShaderVariantEntries[TERRAIN3D_MAX_BUFFER_SHADER_VARIANT_CACHE_LIMIT] = {};
	int m_nBufferShaderVariantCount = 0;
	uint64_t m_nBufferShaderVariantSourceBytes = 0;
	bool m_bBufferShaderOverrideEnabled = false;
	Ref<Shader> m_pBufferShaderOverride; // User's shader we copy code from
	Ref<Shader> m_pConnectedBufferShaderOverride;
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
	Ref<NoiseTexture2D> m_pWorldNoiseCache;

	// Material Features
	t3dworldBackground_t m_worldBackground = FLAT;
	t3dshaderVariantMode_t m_shaderVariantMode = SHADER_VARIANTS_PREWARM_ACTIVE_AND_RECENT;
	t3dshaderPerformancePreset_t m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	t3dprojectionMode_t m_projectionMode = PROJECTION_DISABLED;
	t3dtessellationMaterialMode_t m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
	t3dworldNoiseShaderMode_t m_worldNoiseShaderMode = WORLD_NOISE_DYNAMIC;
	bool m_bDualScalingEnabled = false;
	bool m_bAutoShaderEnabled = false;
	bool m_bMacroVariationEnabled = false;
	t3dtierQualityConfig_t m_nearQuality = { LINEAR, MATERIAL_SAMPLING_FULL,
		TERRAIN_NORMAL_QUALITY_DETAILED, TERRAIN_COLOR_QUALITY_EXACT, SURFACE_RESPONSE_DETAILED,
		TEXTURE_SURFACE_TEXTURED, TIER_LIGHTING_PBR, TIER_DETILING_ROTATION, 1.0f, 1.0f, 0.0f };
	t3dtierQualityConfig_t m_middleQuality = { LINEAR, MATERIAL_SAMPLING_FULL,
		TERRAIN_NORMAL_QUALITY_DETAILED, TERRAIN_COLOR_QUALITY_FAST, SURFACE_RESPONSE_SIMPLE,
		TEXTURE_SURFACE_TEXTURED, TIER_LIGHTING_LAMBERT, TIER_DETILING_ROTATION, 1.0f, 1.0f, 0.0f };
	t3dtierQualityConfig_t m_farQuality = { LINEAR, MATERIAL_SAMPLING_FULL,
		TERRAIN_NORMAL_QUALITY_GEOMETRY, TERRAIN_COLOR_QUALITY_FAST, SURFACE_RESPONSE_SIMPLE,
		TEXTURE_SURFACE_TEXTURED, TIER_LIGHTING_LAMBERT, TIER_DETILING_DISABLED, 1.0f, 1.0f, -0.5f };
#ifdef DEBUG_ENABLED
	uint64_t m_szTierUniformPublicationCount[SHADER_TIER_COUNT] = {};
#endif
	uint64_t m_prewarmSignature = UINT64_MAX;
	int m_nShaderVariantCacheLimit = 32;
	int m_nShaderVariantPrewarmLimit = 8;
	int m_nShaderVariantSourceBudgetMb = 8;
	int m_nShaderVariantEffectiveSourceBudgetMb = 8;
	int m_nLastPrewarmVariantCount = 0;
	int m_nLastPrewarmSkippedCount = 0;
	t3dshaderVariantState_t m_lastBoundVariantState;
	bool m_bHasLastBoundVariantState = false;
	uint64_t m_observedVariantDimensions = 0;
	uint64_t m_szVariantDimensionLastUsed[VARIANT_DIM_COUNT] = {};
	uint64_t m_variantTransitionClock = 0;
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
	t3dshaderTierValidationState_t m_szShaderTierValidation[SHADER_TIER_COUNT] = {};
#ifdef DEBUG_ENABLED
	int m_shaderRegressionInvalidTier = -1;
#endif
	uint64_t m_nShaderUpdateUsecTotal = 0;
	uint64_t m_nShaderUpdateUsecPeak = 0;
	uint64_t m_nShaderUpdateUsecLast = 0;
	uint64_t m_nShaderUpdateCount = 0;
	int m_lastVariantRequestDomain = -1;
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
	String prepareShaderSource(
		const String &rShader, const t3dshaderVariantState_t &rState,
		const Array &rExcludes = Array(), const t3dshaderTier_t tier = SHADER_TIER_NEAR,
		const bool bIncludeEditorFeatures = false
	) const;
	PackedStringArray getEditorShaderFeatures( void ) const;
	t3dshaderVariantState_t captureVariantState( void ) const;
	bool samplesSurfaceData( const t3dshaderVariantState_t &rState ) const;
	void applyPerformancePresetToState( t3dshaderVariantState_t &rState, const t3dshaderPerformancePreset_t preset ) const;
	void applyXboxProfileManifestToState( t3dshaderVariantState_t &rState ) const;
	void applyXboxProfileManifestToTiers( void );
	void applyVariantState( const t3dshaderVariantState_t &rState );
	t3dshaderVariantKey_t makeVariantCacheKey( const String &rCode ) const;
	t3dshaderVariantEntry_t *getShaderVariantEntries( const t3dshaderVariantDomain_t domain );
	const t3dshaderVariantEntry_t *getShaderVariantEntries( const t3dshaderVariantDomain_t domain ) const;
	int &getShaderVariantCountForDomain( const t3dshaderVariantDomain_t domain );
	int getShaderVariantCountForDomain( const t3dshaderVariantDomain_t domain ) const;
	uint64_t &getShaderVariantDomainSourceBytes( const t3dshaderVariantDomain_t domain );
	uint64_t getShaderVariantDomainSourceBytes( const t3dshaderVariantDomain_t domain ) const;
	int getVariantPhysicalCapacity( const t3dshaderVariantDomain_t domain ) const;
	int getVariantCacheLimit( const t3dshaderVariantDomain_t domain ) const;
	uint64_t getShaderVariantSourceBytesRaw( void ) const;
	bool isActiveShader( const Ref<Shader> &rShader ) const;
	int findShaderVariant( const t3dshaderVariantDomain_t domain, const t3dshaderVariantKey_t &rKey ) const;
	void eraseShaderVariant( const t3dshaderVariantDomain_t domain, const int iIndex );
	void touchShaderVariant( const t3dshaderVariantDomain_t domain, const int iIndex, const bool bActualUse );
	bool evictLruShaderVariant( const t3dshaderVariantDomain_t domain, const bool bSpeculativeOnly = false );
	bool evictBudgetLruShaderVariant(
		const t3dshaderVariantDomain_t requestDomain,
		const bool bSpeculativeOnly = false
	);
	void trimShaderVariantCaches( void );
	void submitShaderSource( const Ref<Shader> &rShader, const String &rCode );
	t3dshaderValidationResult_t validateShaderVariant( const Ref<Shader> &rShader, const t3dshaderTier_t tier );
	void recordShaderTierValidation(
		const t3dshaderTier_t tier, const String &rCode,
		const t3dshaderValidationResult_t &rValidation, const uint64_t nActiveShaderRidId,
		const bool bFallbackToNear, const bool bCompactTierActive
	);
	Ref<Shader> getOrCreateShaderVariant(
		const String &rCode, const t3dshaderVariantDomain_t domain,
		const bool bPrewarm = false, const t3dshaderTier_t tier = SHADER_TIER_NEAR,
		t3dshaderValidationResult_t *pValidation = nullptr
	);
	uint64_t getChangedVariantDimensions(
		const t3dshaderVariantState_t &rPrevious,
		const t3dshaderVariantState_t &rCurrent
	) const;
	uint64_t getInferredPotentialDimensions( const t3dshaderVariantState_t &rState ) const;
	void recordVariantTransition( const t3dshaderVariantState_t &rState );
	bool appendVariantState( t3dshaderVariantState_t *pStates, int &nCount, const t3dshaderVariantState_t &rState ) const;
	void appendDimensionStates(
		t3dshaderVariantState_t *pStates, int &nCount, const t3dshaderVariantState_t &rBase,
		const t3dshaderVariantDimension_t dimension
	) const;
	void appendPotentialBranchStates(
		t3dshaderVariantState_t *pStates, int &nCount,
		const t3dshaderVariantState_t &rBase
	) const;
	void prewarmShaderVariantsInternal( const bool bForce = false );
	uint64_t getPrewarmSignature( void ) const;
	String generateShaderCode( void ) const;
	String generateShaderCode( const t3dshaderVariantState_t &rState ) const;
	String generateShaderCode(
		const t3dshaderVariantState_t &rState, const t3dshaderTier_t tier,
		const bool bIncludeEditorFeatures = false
	) const;
	void applyTierQualityToState(
		t3dshaderVariantState_t &rState, const t3dtierQualityConfig_t &rQuality,
		const t3dshaderTier_t tier
	) const;
	void applyPerformancePresetToTiers( const t3dshaderPerformancePreset_t preset );
	void tierQualityChanged( const t3dshaderTier_t tier, const bool bRecompile = true );
	void refreshGpuRegionChannelPolicy( void );
	void updateTierUniforms( const t3dshaderTier_t tier );
	String getTierRangeContext( const t3dshaderTier_t tier ) const;
	String getTierShadowContext( const t3dshaderTier_t tier ) const;
	String getTierShaderStatus( const t3dshaderTier_t tier ) const;
	t3dshaderVariantState_t makeTierState( const t3dshaderVariantState_t &rState, const t3dshaderTier_t tier ) const;
	void setRuntimeMaterialParam( const StringName &rName, const Variant &rValue );
	String generateBufferShaderCode( void ) const;
	String generateBufferShaderCode( const t3dshaderVariantState_t &rState ) const;
	String finalizeRuntimeShaderCode( const String &rShader ) const;
	String stripComments( const String &rShader ) const;
	String injectEditorCode( const String &rShader ) const;
	void updateShader( void );
	void refreshShaderPropertyCache( void );
	void disconnectShaderOverrideSignals( void );
	void connectShaderOverrideSignals( void );
	void ensureWorldNoiseCache( void );
	typedef enum t3dpageTableBuildResult_e {
		PAGE_TABLE_READY,
		PAGE_TABLE_INVALID_TERRAIN_STATE,
		PAGE_TABLE_INVALID_DIMENSIONS,
		PAGE_TABLE_TEXTURE_ALLOCATION_FAILED,
	} t3dpageTableBuildResult_t;
	void invalidateRegionPageTables( void );
	bool updateRegionMapTexture( const PackedInt32Array &rRegionMap, int nRegionMapSize );
	bool updateLogicalRegionMapTexture( const PackedInt32Array &rRegionMap, int nRegionMapSize );
	t3dpageTableBuildResult_t prepareRegionPageTables( void );
	void bindRegionPageTableUniforms( const RID &rMaterial );
	void updateFarAtlasUniforms( const RID &rMaterial );
	void updateUniforms( const RID &rMaterial, const uint32_t nUpdate = UNIFORMS_ONLY );
	void setShaderParameters( const Dictionary &rDict );
	Dictionary getShaderParameters( void ) const {
		return m_shaderParams;
	}

public:
	T3DMaterial( void ) {}
	~T3DMaterial( void ) {
		destroy();
	}
	void initialize( T3DTerrain *pTerrain );
	bool isInitialized( void ) const {
		return m_pTerrain != nullptr;
	}
	bool isInitializedFor( const T3DTerrain *pTerrain ) const {
		return m_pTerrain == pTerrain;
	}
	T3DTerrain *getTerrainOwner( void ) const {
		return m_pTerrain;
	}
	void uninitialize( T3DTerrain *pTerrain );
	void destroy( void );

	void update( const uint32_t nFlags = UNIFORMS_ONLY );
	void updateEditorOverlay( void );
	bool isRenderShaderReady( void ) const {
		return m_bNearShaderReady && m_pShader.is_valid() && m_pShader->get_rid().is_valid();
	}
	void updateRegionPageTable( void );
	bool isRegionPageTableValid( void ) const {
		return m_generatedRegionMap.getRid().is_valid() && m_generatedLogicalRegionMap.getRid().is_valid() &&
			m_nRegionMapTextureSize > 0 && m_nLogicalRegionMapTextureSize == m_nRegionMapTextureSize;
	}
	void updateFarAtlasTextures( void );
	RID getMaterialRid( void ) const {
		return m_pMaterial;
	}
	RID getMaterialRidForTier( const t3dshaderTier_t tier ) const {
		if ( tier <= SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT || !m_szTierMaterials[tier].is_valid() ) {
			return m_pMaterial;
		}
		return m_szTierMaterials[tier];
	}
	RID getShaderRidForTier( const t3dshaderTier_t tier ) const {
		if ( tier <= SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT || m_szTierShaders[tier].is_null() ) {
			return getShaderRid();
		}
		return m_szTierShaders[tier]->get_rid();
	}
	RID getShaderRid( void ) const {
		return m_pShader.is_valid() ? m_pShader->get_rid() : RID();
	}

	RID getBufferMaterialRid( void ) const {
		return m_bufferMaterial;
	}
	RID getBufferShaderRid( void ) const {
		return m_pBufferShader.is_valid() ? m_pBufferShader->get_rid() : RID();
	}

	// Material settings
	void setDisplacementScale( const real_t fDisplacementScale );
	real_t getDisplacementScale( void ) const {
		return m_fDisplacementScale;
	}
	void setDisplacementSharpness( const real_t fDisplacementSharpness );
	real_t getDisplacementSharpness( void ) const {
		return m_fDisplacementSharpness;
	}
	void setWorldBackground( const t3dworldBackground_t background );
	t3dworldBackground_t getWorldBackground( void ) const {
		return m_worldBackground;
	}
	// PATCH-02 canonical 11-row tier schema.
	void setNearTextureFiltering( t3dtextureFiltering_t filtering );
	t3dtextureFiltering_t getNearTextureFiltering( void ) const { return m_nearQuality.textureFiltering; }
	void setNearMaterialSamplingQuality( t3dmaterialSamplingQuality_t eQuality );
	t3dmaterialSamplingQuality_t getNearMaterialSamplingQuality( void ) const { return m_nearQuality.materialSamplingQuality; }
	void setNearTerrainNormalQuality( t3dterrainNormalQuality_t eQuality );
	t3dterrainNormalQuality_t getNearTerrainNormalQuality( void ) const { return m_nearQuality.terrainNormalQuality; }
	void setNearTerrainColorQuality( t3dterrainColorQuality_t eQuality );
	t3dterrainColorQuality_t getNearTerrainColorQuality( void ) const { return m_nearQuality.terrainColorQuality; }
	void setNearSurfaceResponseQuality( t3dsurfaceResponseQuality_t eQuality );
	t3dsurfaceResponseQuality_t getNearSurfaceResponseQuality( void ) const { return m_nearQuality.surfaceResponseQuality; }
	void setNearTextureSurfaceMode( t3dtextureSurfaceMode_t mode );
	t3dtextureSurfaceMode_t getNearTextureSurfaceMode( void ) const { return m_nearQuality.textureSurfaceMode; }
	void setNearLightingModel( t3dtierLightingModel_t model );
	t3dtierLightingModel_t getNearLightingModel( void ) const { return m_nearQuality.lightingModel; }
	void setNearDetilingQuality( t3dtierDetilingQuality_t eQuality );
	t3dtierDetilingQuality_t getNearDetilingQuality( void ) const { return m_nearQuality.detilingQuality; }
	void setNearAuthoredColorContribution( real_t fContribution );
	real_t getNearAuthoredColorContribution( void ) const { return m_nearQuality.fAuthoredColorContribution; }
	void setNearWetnessContribution( real_t fContribution );
	real_t getNearWetnessContribution( void ) const { return m_nearQuality.fWetnessContribution; }
	void setNearMaterialLodBias( real_t fBias );
	real_t getNearMaterialLodBias( void ) const { return m_nearQuality.fMaterialLodBias; }

	void setMiddleMaterialSamplingQuality( t3dmaterialSamplingQuality_t eQuality );
	t3dmaterialSamplingQuality_t getMiddleMaterialSamplingQuality( void ) const { return m_middleQuality.materialSamplingQuality; }
	void setMiddleTerrainNormalQuality( t3dterrainNormalQuality_t eQuality );
	t3dterrainNormalQuality_t getMiddleTerrainNormalQuality( void ) const { return m_middleQuality.terrainNormalQuality; }
	void setMiddleTerrainColorQuality( t3dterrainColorQuality_t eQuality );
	t3dterrainColorQuality_t getMiddleTerrainColorQuality( void ) const { return m_middleQuality.terrainColorQuality; }
	void setMiddleSurfaceResponseQuality( t3dsurfaceResponseQuality_t eQuality );
	t3dsurfaceResponseQuality_t getMiddleSurfaceResponseQuality( void ) const { return m_middleQuality.surfaceResponseQuality; }
	void setMiddleLightingModel( t3dtierLightingModel_t model );
	t3dtierLightingModel_t getMiddleLightingModel( void ) const { return m_middleQuality.lightingModel; }
	void setMiddleDetilingQuality( t3dtierDetilingQuality_t eQuality );
	t3dtierDetilingQuality_t getMiddleDetilingQuality( void ) const { return m_middleQuality.detilingQuality; }
	void setMiddleAuthoredColorContribution( real_t fContribution );
	real_t getMiddleAuthoredColorContribution( void ) const { return m_middleQuality.fAuthoredColorContribution; }
	void setMiddleWetnessContribution( real_t fContribution );
	real_t getMiddleWetnessContribution( void ) const { return m_middleQuality.fWetnessContribution; }

	void setFarMaterialSamplingQuality( t3dmaterialSamplingQuality_t eQuality );
	t3dmaterialSamplingQuality_t getFarMaterialSamplingQuality( void ) const { return m_farQuality.materialSamplingQuality; }
	void setFarTerrainNormalQuality( t3dterrainNormalQuality_t eQuality );
	t3dterrainNormalQuality_t getFarTerrainNormalQuality( void ) const { return m_farQuality.terrainNormalQuality; }
	void setFarTerrainColorQuality( t3dterrainColorQuality_t eQuality );
	t3dterrainColorQuality_t getFarTerrainColorQuality( void ) const { return m_farQuality.terrainColorQuality; }
	void setFarSurfaceResponseQuality( t3dsurfaceResponseQuality_t eQuality );
	t3dsurfaceResponseQuality_t getFarSurfaceResponseQuality( void ) const { return m_farQuality.surfaceResponseQuality; }
	void setFarLightingModel( t3dtierLightingModel_t model );
	t3dtierLightingModel_t getFarLightingModel( void ) const { return m_farQuality.lightingModel; }
	void setFarDetilingQuality( t3dtierDetilingQuality_t eQuality );
	t3dtierDetilingQuality_t getFarDetilingQuality( void ) const { return m_farQuality.detilingQuality; }
	void setFarAuthoredColorContribution( real_t fContribution );
	real_t getFarAuthoredColorContribution( void ) const { return m_farQuality.fAuthoredColorContribution; }
	void setFarWetnessContribution( real_t fContribution );
	real_t getFarWetnessContribution( void ) const { return m_farQuality.fWetnessContribution; }
	void setFarMaterialLodBias( real_t fBias );
	real_t getFarMaterialLodBias( void ) const { return m_farQuality.fMaterialLodBias; }

	String getNearRangeContext( void ) const { return getTierRangeContext( SHADER_TIER_NEAR ); }
	String getMiddleRangeContext( void ) const { return getTierRangeContext( SHADER_TIER_MIDDLE ); }
	String getFarRangeContext( void ) const { return getTierRangeContext( SHADER_TIER_FAR ); }
	String getNearShadowContext( void ) const { return getTierShadowContext( SHADER_TIER_NEAR ); }
	String getMiddleShadowContext( void ) const { return getTierShadowContext( SHADER_TIER_MIDDLE ); }
	String getFarShadowContext( void ) const { return getTierShadowContext( SHADER_TIER_FAR ); }
	String getNearShaderStatus( void ) const { return getTierShaderStatus( SHADER_TIER_NEAR ); }
	String getMiddleShaderStatus( void ) const { return getTierShaderStatus( SHADER_TIER_MIDDLE ); }
	String getFarShaderStatus( void ) const { return getTierShaderStatus( SHADER_TIER_FAR ); }

	// Hidden serialized compatibility aliases. Runtime state is owned only by the
	// canonical t3dtierQualityConfig_t rows above.
	void setTextureFiltering( const t3dtextureFiltering_t filtering );
	t3dtextureFiltering_t getTextureFiltering( void ) const {
		return m_nearQuality.textureFiltering;
	}
	void setShaderVariantMode( const t3dshaderVariantMode_t mode );
	t3dshaderVariantMode_t getShaderVariantMode( void ) const {
		return m_shaderVariantMode;
	}
	void setShaderVariantCacheLimit( const int nLimit );
	int getShaderVariantCacheLimit( void ) const {
		return m_nShaderVariantCacheLimit;
	}
	void setShaderVariantPrewarmLimit( const int nLimit );
	int getShaderVariantPrewarmLimit( void ) const {
		return m_nShaderVariantPrewarmLimit;
	}
	void setShaderVariantSourceBudgetMb( const int nBudgetMb );
	int getShaderVariantSourceBudgetMb( void ) const {
		return m_nShaderVariantSourceBudgetMb;
	}
	void setEffectiveShaderVariantSourceBudgetMb( int nBudgetMb );
	int getEffectiveShaderVariantSourceBudgetMb( void ) const {
		return m_nShaderVariantEffectiveSourceBudgetMb;
	}
	size_t getShaderVariantSourceBytesNative( void ) const {
		return getShaderVariantSourceBytesRaw();
	}
	void setShaderPerformancePreset( const t3dshaderPerformancePreset_t preset );
	t3dshaderPerformancePreset_t getShaderPerformancePreset( void ) const {
		return m_shaderPerformancePreset;
	}
	// Middle tier quality. Existing unprefixed quality properties remain the Near tier.
	void setMiddleTextureFiltering( t3dtextureFiltering_t filtering );
	t3dtextureFiltering_t getMiddleTextureFiltering( void ) const { return m_middleQuality.textureFiltering; }
	void setMiddleTerrainSamplingMode( t3dterrainSamplingMode_t mode );
	t3dterrainSamplingMode_t getMiddleTerrainSamplingMode( void ) const;
	void setMiddleTerrainNormalMode( t3dterrainNormalMode_t mode );
	t3dterrainNormalMode_t getMiddleTerrainNormalMode( void ) const;
	void setMiddleColorSamplingMode( t3dcolorSamplingMode_t mode );
	t3dcolorSamplingMode_t getMiddleColorSamplingMode( void ) const;
	void setMiddleSurfaceDataMode( t3dsurfaceDataMode_t mode );
	t3dsurfaceDataMode_t getMiddleSurfaceDataMode( void ) const;
	void setMiddleTextureSurfaceMode( t3dtextureSurfaceMode_t mode );
	t3dtextureSurfaceMode_t getMiddleTextureSurfaceMode( void ) const { return m_middleQuality.textureSurfaceMode; }
	void setMiddleTerrainShadingMode( t3dterrainShadingMode_t mode );
	t3dterrainShadingMode_t getMiddleTerrainShadingMode( void ) const;
	void setMiddleDetilingMode( t3ddetilingMode_t mode );
	t3ddetilingMode_t getMiddleDetilingMode( void ) const;
	void setMiddleAutoShaderEnabled( bool bEnabled );
	bool getMiddleAutoShaderEnabled( void ) const { return m_bAutoShaderEnabled; }
	void setMiddleAuthoredColorEnabled( bool bEnabled );
	bool getMiddleAuthoredColorEnabled( void ) const { return m_middleQuality.fAuthoredColorContribution > 0.0f; }
	void setMiddleWetnessEnabled( bool bEnabled );
	bool getMiddleWetnessEnabled( void ) const { return m_middleQuality.fWetnessContribution > 0.0f; }
	void setMiddleMaterialLodBias( real_t fBias );
	real_t getMiddleMaterialLodBias( void ) const { return m_middleQuality.fMaterialLodBias; }

	// Far tier quality. The far shader intentionally remains a dominant-material, one-albedo-sample path.
	void setFarTextureFiltering( t3dtextureFiltering_t filtering );
	t3dtextureFiltering_t getFarTextureFiltering( void ) const { return m_farQuality.textureFiltering; }
	void setFarTextureSurfaceMode( t3dtextureSurfaceMode_t mode );
	t3dtextureSurfaceMode_t getFarTextureSurfaceMode( void ) const { return m_farQuality.textureSurfaceMode; }
	void setFarTerrainShadingMode( t3dterrainShadingMode_t mode );
	t3dterrainShadingMode_t getFarTerrainShadingMode( void ) const;
	void setFarMaterialLod( real_t fLod );
	real_t getFarMaterialLod( void ) const { return m_farQuality.fMaterialLodBias + 4.0f; }
	void setFarAuthoredColorEnabled( bool bEnabled );
	bool getFarAuthoredColorEnabled( void ) const { return m_farQuality.fAuthoredColorContribution > 0.0f; }
	void setFarWetnessEnabled( bool bEnabled );
	bool getFarWetnessEnabled( void ) const { return m_farQuality.fWetnessContribution > 0.0f; }

	void setTerrainSamplingMode( const t3dterrainSamplingMode_t mode );
	t3dterrainSamplingMode_t getTerrainSamplingMode( void ) const {
		return m_nearQuality.materialSamplingQuality == MATERIAL_SAMPLING_FAST ?
			TERRAIN_SAMPLING_SINGLE_SAMPLE : TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED;
	}
	void setTerrainNormalMode( const t3dterrainNormalMode_t mode );
	t3dterrainNormalMode_t getTerrainNormalMode( void ) const {
		return m_nearQuality.terrainNormalQuality == TERRAIN_NORMAL_QUALITY_GEOMETRY ?
			TERRAIN_NORMAL_GEOMETRY_DERIVATIVE : TERRAIN_NORMAL_DERIVED_TEXTURE;
	}
	void setColorSamplingMode( const t3dcolorSamplingMode_t mode );
	t3dcolorSamplingMode_t getColorSamplingMode( void ) const {
		return m_nearQuality.terrainColorQuality == TERRAIN_COLOR_QUALITY_FAST ?
			COLOR_SAMPLING_HARDWARE_FILTERED : COLOR_SAMPLING_EXACT_4_TAP;
	}
	void setProjectionMode( const t3dprojectionMode_t mode );
	t3dprojectionMode_t getProjectionMode( void ) const {
		return m_projectionMode;
	}
	void setTessellationMaterialMode( const t3dtessellationMaterialMode_t mode );
	t3dtessellationMaterialMode_t getTessellationMaterialMode( void ) const {
		return m_tessellationMaterialMode;
	}
	void setSurfaceDataMode( const t3dsurfaceDataMode_t mode );
	t3dsurfaceDataMode_t getSurfaceDataMode( void ) const {
		return m_nearQuality.surfaceResponseQuality == SURFACE_RESPONSE_SIMPLE ?
			SURFACE_DATA_ALBEDO_ONLY : SURFACE_DATA_FULL;
	}
	void setTextureSurfaceMode( const t3dtextureSurfaceMode_t mode );
	t3dtextureSurfaceMode_t getTextureSurfaceMode( void ) const {
		return m_nearQuality.textureSurfaceMode;
	}
	void setTerrainShadingMode( const t3dterrainShadingMode_t mode );
	t3dterrainShadingMode_t getTerrainShadingMode( void ) const {
		return m_nearQuality.lightingModel == TIER_LIGHTING_UNSHADED ? SHADING_UNSHADED :
			( m_nearQuality.lightingModel == TIER_LIGHTING_BLINN_PHONG ?
				SHADING_XBOX_360_BLINN_PHONG : ( m_nearQuality.lightingModel == TIER_LIGHTING_PBR ?
					SHADING_BURLEY_GGX : SHADING_LAMBERT_DIFFUSE_ONLY ) );
	}
	void setDetilingMode( const t3ddetilingMode_t mode );
	t3ddetilingMode_t getDetilingMode( void ) const {
		return m_nearQuality.detilingQuality == TIER_DETILING_DISABLED ?
			DETILING_DISABLED : DETILING_DISCRETE_8;
	}
	void setWorldNoiseShaderMode( const t3dworldNoiseShaderMode_t mode );
	t3dworldNoiseShaderMode_t getWorldNoiseShaderMode( void ) const {
		return m_worldNoiseShaderMode;
	}
	void prewarmShaderVariants( void );
	void clearShaderVariantCache( void );
	int getShaderVariantCount( void ) const {
		return m_nRuntimeShaderVariantCount + m_nEditorShaderVariantCount;
	}
	int getRuntimeShaderVariantCount( void ) const {
		return m_nRuntimeShaderVariantCount;
	}
	int getEditorShaderVariantCount( void ) const {
		return m_nEditorShaderVariantCount;
	}
	int getBufferShaderVariantCount( void ) const {
		return m_nBufferShaderVariantCount;
	}
	int getSpeculativeShaderVariantCount( void ) const;
	int64_t getShaderVariantSourceBytes( void ) const {
		return ( int64_t ) ( getShaderVariantSourceBytesRaw() );
	}
	int getLastPrewarmVariantCount( void ) const {
		return m_nLastPrewarmVariantCount;
	}
	int getLastPrewarmSkippedCount( void ) const {
		return m_nLastPrewarmSkippedCount;
	}
	int64_t getTheoreticalShaderVariantCount( void ) const {
		return 1492992000LL;
	}
	Dictionary getShaderVariantDiagnostics( void ) const;
	Dictionary getShaderTierDiagnostics( const int nTier ) const;
	void resetShaderVariantDiagnostics( void );
	String getShaderRegressionSource(
		const int nTier, const int nAssemblyPath,
		const bool bControlMapsImplicit, const bool bColorMapsImplicit
	);
#ifdef DEBUG_ENABLED
	void setShaderRegressionInvalidTier( const int nTier );
	Variant getTierRegressionMaterialParam( const int nTier, const StringName &rName ) const;
#endif
	uint64_t getShaderUpdateCount( void ) const {
		return m_nShaderUpdateCount;
	}
	uint64_t getShaderVariantCreationCount( void ) const {
		return m_nVariantCreationsTotal;
	}
	uint64_t getShaderSourceSubmissionUsecLast( void ) const {
		return m_nVariantSourceSubmissionUsecLast;
	}
	uint64_t getShaderUpdateUsecLast( void ) const {
		return m_nShaderUpdateUsecLast;
	}
	void setPackedPbrSingleFetch( const bool bEnabled );
	bool getPackedPbrSingleFetch( void ) const {
		return false;
	}
	void setAutoShaderEnabled( const bool bEnabled );
	// Editor preview helper. This changes only the active shader state and does
	// not alter the serialized performance preset or dirty the material resource.
	void setAutoShaderEnabledTransient( const bool bEnabled );
	bool getAutoShaderEnabled( void ) const {
		return m_bAutoShaderEnabled;
	}
	void setDualScalingEnabled( const bool bEnabled );
	bool getDualScalingEnabled( void ) const {
		return m_bDualScalingEnabled;
	}
	void setMacroVariationEnabled( const bool bEnabled );
	bool getMacroVariationEnabled( void ) const {
		return m_bMacroVariationEnabled;
	}
	void setProjectionEnabled( const bool bEnabled );
	bool getProjectionEnabled( void ) const {
		return m_projectionMode != PROJECTION_DISABLED;
	}

	void setShaderOverrideEnabled( const bool bEnabled );
	bool isShaderOverrideEnabled( void ) const {
		return m_bShaderOverrideEnabled;
	}
	void setShaderOverride( const Ref<Shader> &rShader );
	Ref<Shader> getShaderOverride( void ) const {
		return m_pShaderOverride;
	}

	void setBufferShaderOverrideEnabled( const bool bEnabled );
	bool isBufferShaderOverrideEnabled( void ) const {
		return m_bBufferShaderOverrideEnabled;
	}
	void setBufferShaderOverride( const Ref<Shader> &rShader );
	Ref<Shader> getBufferShaderOverride( void ) const {
		return m_pBufferShaderOverride;
	}

	void setShaderParam( const StringName &rName, const Variant &rValue );
	Variant getShaderParam( const StringName &rName ) const;

	// PBR outputs
	void setOutputAlbedoEnabled( const bool bEnabled );
	bool getOutputAlbedoEnabled( void ) const {
		return m_bOutputAlbedoEnabled;
	}
	void setOutputRoughnessEnabled( const bool bEnabled );
	bool getOutputRoughnessEnabled( void ) const {
		return m_bOutputRoughnessEnabled;
	}
	void setOutputSpecularEnabled( const bool bEnabled );
	bool getOutputSpecularEnabled( void ) const {
		return m_bOutputSpecularEnabled;
	}
	void setOutputNormalMapEnabled( const bool bEnabled );
	bool getOutputNormalMapEnabled( void ) const {
		return m_bOutputNormalMapEnabled;
	}
	void setOutputAmbientOcclusionEnabled( const bool bEnabled );
	bool getOutputAmbientOcclusionEnabled( void ) const {
		return m_bOutputAmbientOcclusionEnabled;
	}

	// Overlays
	void setShowRegionGrid( const bool bEnabled );
	bool getShowRegionGrid( void ) const {
		return m_bShowRegionGrid;
	}
	void setShowInstancerGrid( const bool bEnabled );
	bool getShowInstancerGrid( void ) const {
		return m_bShowInstancerGrid;
	}
	void setShowVertexGrid( const bool bEnabled );
	bool getShowVertexGrid( void ) const {
		return m_bShowVertexGrid;
	}
	void setShowContours( const bool bEnabled );
	bool getShowContours( void ) const {
		return m_bShowContours;
	}
	void setShowNavigation( const bool bEnabled );
	bool getShowNavigation( void ) const {
		return m_bShowNavigation;
	}

	// Debug views
	void setShowCheckered( const bool bEnabled );
	bool getShowCheckered( void ) const {
		return m_bDebugViewCheckered;
	}
	void setShowGrey( const bool bEnabled );
	bool getShowGrey( void ) const {
		return m_bDebugViewGrey;
	}
	void setShowHeightmap( const bool bEnabled );
	bool getShowHeightmap( void ) const {
		return m_bDebugViewHeightmap;
	}
	void setShowJaggedness( const bool bEnabled );
	bool getShowJaggedness( void ) const {
		return m_bDebugViewJaggedness;
	}
	void setShowAutoshader( const bool bEnabled );
	bool getShowAutoshader( void ) const {
		return m_bDebugViewAutoshader;
	}
	void setShowControlTexture( const bool bEnabled );
	bool getShowControlTexture( void ) const {
		return m_bDebugViewControlTexture;
	}
	void setShowControlBlend( const bool bEnabled );
	bool getShowControlBlend( void ) const {
		return m_bDebugViewControlBlend;
	}
	void setShowControlAngle( const bool bEnabled );
	bool getShowControlAngle( void ) const {
		return m_bDebugViewControlAngle;
	}
	void setShowControlScale( const bool bEnabled );
	bool getShowControlScale( void ) const {
		return m_bDebugViewControlScale;
	}
	void setShowColormap( const bool bEnabled );
	bool getShowColormap( void ) const {
		return m_bDebugViewColormap;
	}
	void setShowRoughmap( const bool bEnabled );
	bool getShowRoughmap( void ) const {
		return m_bDebugViewRoughmap;
	}
	void setShowDisplacementBuffer( const bool bEnabled );
	bool getShowDisplacementBuffer( void ) const {
		return m_bDebugViewDisplacementBuffer;
	}

	// PBR Views
	void setShowTextureAlbedo( const bool bEnabled );
	bool getShowTextureAlbedo( void ) const {
		return m_bPbrViewTexAlbedo;
	}
	void setShowTextureHeight( const bool bEnabled );
	bool getShowTextureHeight( void ) const {
		return m_bPbrViewTexHeight;
	}
	void setShowTextureNormal( const bool bEnabled );
	bool getShowTextureNormal( void ) const {
		return m_bPbrViewTexNormal;
	}
	void setShowTextureRough( const bool bEnabled );
	bool getShowTextureRough( void ) const {
		return m_bPbrViewTexRough;
	}
	void setShowTextureAo( const bool bEnabled );
	bool getShowTextureAo( void ) const {
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

VARIANT_ENUM_CAST( T3DMaterial::t3dworldBackground_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dtextureFiltering_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dupdateFlags_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dshaderVariantMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dshaderPerformancePreset_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dterrainSamplingMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dterrainNormalMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dcolorSamplingMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dprojectionMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dtessellationMaterialMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dsurfaceDataMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dtextureSurfaceMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dterrainShadingMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3ddetilingMode_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dmaterialSamplingQuality_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dterrainNormalQuality_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dterrainColorQuality_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dsurfaceResponseQuality_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dtierLightingModel_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dtierDetilingQuality_t );
VARIANT_ENUM_CAST( T3DMaterial::t3dworldNoiseShaderMode_t );

using Terrain3DMaterial = T3DMaterial;

#endif // TERRAIN3D_MATERIAL_CLASS_H
