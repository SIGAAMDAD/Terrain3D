// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_SHADER_SPECIALIZER_H
#define TERRAIN3D_SHADER_SPECIALIZER_H

#include <stdint.h>

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

// WG-A P01-P03 frozen terrain shader pipeline contract. Keep this key bounded:
// only renderer-reachable combinations may be materialized as RDShaderFile versions.
typedef enum t3dTerrainShaderPassClass_e {
	T3D_TERRAIN_SHADER_PASS_COLOR_GENERAL = 0u,
	T3D_TERRAIN_SHADER_PASS_COLOR_ONE_SUN = 1u,
	T3D_TERRAIN_SHADER_PASS_DEPTH_ONLY = 2u,
	T3D_TERRAIN_SHADER_PASS_DEPTH_NORMAL_ROUGHNESS = 3u,
	T3D_TERRAIN_SHADER_PASS_COUNT = 4u,
} t3dTerrainShaderPassClass_t;

typedef enum t3dTerrainMaterialBackend_e {
	T3D_TERRAIN_MATERIAL_BACKEND_CANONICAL_VT_V3 = 0u,
	T3D_TERRAIN_MATERIAL_BACKEND_COMPATIBILITY_MIXED = 1u,
	T3D_TERRAIN_MATERIAL_BACKEND_COUNT = 2u,
} t3dTerrainMaterialBackend_t;

typedef enum t3dTerrainShaderPath_e {
	// Normal runtime path. Legacy/editor material formats are not reachable.
	T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL = 0u,
	// Runtime/import fallback for mixed or uncooked material publication.
	T3D_TERRAIN_SHADER_PATH_COMPATIBILITY_MIXED = 1u,
	// Editor/debug path. Keeps visualization/decal behavior and mixed inputs.
	T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED = 2u,
	T3D_TERRAIN_SHADER_PATH_COUNT = 3u,
} t3dTerrainShaderPath_t;

// WG-C bounded draw-wide specialization. These are the only material feature
// decisions specialized in this wave; all other feature flags remain runtime.
// The 3-bit domain is intentionally exhaustive and bounded: 0..7.
typedef enum t3dTerrainShippingFeatureClass_e {
	T3D_TERRAIN_SHIPPING_FEATURE_AUTO_SHADER = 1u << 0u,
	T3D_TERRAIN_SHIPPING_FEATURE_DUAL_SCALE = 1u << 1u,
	T3D_TERRAIN_SHIPPING_FEATURE_MEGAVT = 1u << 2u,
	T3D_TERRAIN_SHIPPING_FEATURE_MASK = T3D_TERRAIN_SHIPPING_FEATURE_AUTO_SHADER | T3D_TERRAIN_SHIPPING_FEATURE_DUAL_SCALE
	        | T3D_TERRAIN_SHIPPING_FEATURE_MEGAVT,
	// Compatibility/editor paths deliberately preserve runtime feature tests.
	T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME = 0xffu,
} t3dTerrainShippingFeatureClass_t;

// Shared fragment-stage specialization constant ID. Keep this stable so host
// pipeline creation and the GLSL contract cannot silently drift.
static const uint32_t T3D_TERRAIN_SPEC_CONSTANT_SHIPPING_FEATURE_CLASS = 24u;
static const uint32_t T3D_TERRAIN_SHIPPING_FEATURE_CLASS_COUNT = 8u;
static_assert( T3D_TERRAIN_SHIPPING_FEATURE_MASK == T3D_TERRAIN_SHIPPING_FEATURE_CLASS_COUNT - 1u,
        "Terrain shipping specialization feature bits must remain a dense bounded 0..7 domain" );

// WG-G P14 bounded lighting specialization. Lighting model is tier-stable for
// a terrain draw and therefore belongs in pipeline specialization rather than
// the per-light BRDF branch graph. Compatibility/editor paths retain runtime
// model selection through the explicit 0xff sentinel.
typedef enum t3dTerrainLightingModelClass_e {
	T3D_TERRAIN_LIGHTING_MODEL_UNSHADED = 0u,
	T3D_TERRAIN_LIGHTING_MODEL_LAMBERT = 1u,
	T3D_TERRAIN_LIGHTING_MODEL_BLINN_PHONG = 2u,
	T3D_TERRAIN_LIGHTING_MODEL_PBR = 3u,
	T3D_TERRAIN_LIGHTING_MODEL_RUNTIME = 0xffu,
} t3dTerrainLightingModelClass_t;

// General Forward+ keeps dynamic clustered integrations when either decals or
// reflection probes are present. A second bounded class proves both systems
// absent and lets specialization delete both cluster walks. One-sun always
// selects NONE because those descriptors are already absent from that family.
typedef enum t3dTerrainSceneIntegrationClass_e {
	T3D_TERRAIN_SCENE_INTEGRATION_DYNAMIC = 0u,
	T3D_TERRAIN_SCENE_INTEGRATION_NONE = 1u,
	T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME = 0xffu,
} t3dTerrainSceneIntegrationClass_t;

static const uint32_t T3D_TERRAIN_SPEC_CONSTANT_LIGHTING_MODEL_CLASS = 25u;
static const uint32_t T3D_TERRAIN_SPEC_CONSTANT_SCENE_INTEGRATION_CLASS = 26u;
static const uint32_t T3D_TERRAIN_LIGHTING_MODEL_CLASS_COUNT = 4u;
static const uint32_t T3D_TERRAIN_SCENE_INTEGRATION_CLASS_COUNT = 2u;

// WG-H P11/P12 bounded prepass/middle-tier specialization. DNR outputs are
// draw/tier stable; middle surface source is draw stable and selects either
// the authoritative baked composite or the exact detailed fallback.
typedef enum t3dTerrainDnrOutputClass_e {
	T3D_TERRAIN_DNR_OUTPUT_NORMAL = 1u << 0u,
	T3D_TERRAIN_DNR_OUTPUT_ROUGHNESS = 1u << 1u,
	T3D_TERRAIN_DNR_OUTPUT_MASK = T3D_TERRAIN_DNR_OUTPUT_NORMAL | T3D_TERRAIN_DNR_OUTPUT_ROUGHNESS,
	T3D_TERRAIN_DNR_OUTPUT_RUNTIME = 0xffu,
} t3dTerrainDnrOutputClass_t;

typedef enum t3dTerrainMiddleSurfaceClass_e {
	T3D_TERRAIN_MIDDLE_SURFACE_DETAILED = 0u,
	T3D_TERRAIN_MIDDLE_SURFACE_COMPOSITE = 1u,
	T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME = 0xffu,
} t3dTerrainMiddleSurfaceClass_t;

static const uint32_t T3D_TERRAIN_SPEC_CONSTANT_DNR_OUTPUT_CLASS = 27u;
static const uint32_t T3D_TERRAIN_SPEC_CONSTANT_MIDDLE_SURFACE_CLASS = 28u;
static const uint32_t T3D_TERRAIN_DNR_OUTPUT_CLASS_COUNT = 4u;
static const uint32_t T3D_TERRAIN_MIDDLE_SURFACE_CLASS_COUNT = 2u;

typedef struct t3dTerrainShaderPipelineKey_s {
	uint8_t m_nPassClass = T3D_TERRAIN_SHADER_PASS_COLOR_GENERAL;
	uint8_t m_nTier = 0u;
	uint8_t m_nShaderPath = T3D_TERRAIN_SHADER_PATH_COMPATIBILITY_MIXED;
	uint8_t m_nMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_COMPATIBILITY_MIXED;
	uint8_t m_nShippingFeatureClass = T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME;
	uint8_t m_nLightingModelClass = T3D_TERRAIN_LIGHTING_MODEL_RUNTIME;
	uint8_t m_nSceneIntegrationClass = T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME;
	uint8_t m_nDnrOutputClass = T3D_TERRAIN_DNR_OUTPUT_RUNTIME;
	uint8_t m_nMiddleSurfaceClass = T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;
	bool m_bDisplacement = false;
	bool m_bWorldNoiseFragmentNormal = false;
	bool m_bClusteredDecals = true;
	bool m_bReflectionProbes = true;
	bool m_bReceiveShadows = true;
} t3dTerrainShaderPipelineKey_t;

bool t3dTerrainShaderPipelineKeyIsValid( const t3dTerrainShaderPipelineKey_t &rKey );

typedef struct t3dShaderStaticState_s {
	int m_nTerrainSamplingMode = 0;
	int m_nTerrainNormalMode = 0;
	int m_nColorSamplingMode = 0;
	int m_nProjectionMode = 0;
	int m_nTextureSurfaceMode = 0;
	int m_nTerrainShadingMode = 0;
	int m_nDetilingMode = 0;
	int m_nWorldNoiseShaderMode = 0;
	bool m_bSampleSurfaceData = false;
	bool m_bOutputRoughness = false;
	bool m_bOutputSpecular = false;
	bool m_bOutputNormalMap = false;
	bool m_bOutputAo = false;
	bool m_bFastBlendCurve = false;
	bool m_bControlMapsImplicit = false;
	bool m_bColorMapsImplicit = false;
	bool m_bAutoShaderEnabled = false;
} t3dShaderStaticState_t;

// Reads a root Terrain3D shader template and recursively expands every shader
// include into one deterministic source unit. Repeated include occurrences are
// preserved because a few modules are intentionally used as inline snippets.
bool t3dExpandShaderIncludes( const godot::String &rRootPath, godot::String &rOutputSource, godot::String &rFailureSummary );

// Resolves every Terrain3D-owned compile-time branch in an already-expanded
// source unit. Renderer/platform directives and locally-derived shader macros
// remain for Godot, but no T3D_* preprocessing state may survive this pass.
bool t3dAssembleShaderSource( const godot::String &rExpandedSource,
        const t3dShaderStaticState_t &rState,
        const godot::PackedStringArray &rEnabledFeatures,
        godot::String &rOutputSource,
        godot::String &rFailureSummary );

// Enforces the final built-in shader contract before Shader::set_code().
bool t3dValidateAssembledShaderSource( const godot::String &rSource, godot::String &rFailureSummary );

#endif // TERRAIN3D_SHADER_SPECIALIZER_H
