// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_TYPES_H
#define TERRAIN3D_RD_TYPES_H

#include "Terrain3DConstants.h"

#include <cstddef>
#include <cstdint>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

static constexpr uint32_t s_nRdMaterialLayerCount = 32u;
static constexpr uint32_t s_nRdDrawGroupCount = 4u;
static constexpr uint32_t s_nRdFrameSnapshotSlotCount = 3u;
static constexpr uint32_t s_nRdInvalidFrameSnapshotIndex = s_nRdFrameSnapshotSlotCount;
static constexpr uint64_t s_nRdFrameSnapshotTokenIndexMask = 0x3u;
static constexpr uint64_t s_nRdFrameSnapshotTokenPublishedMask = uint64_t( 1u ) << 63u;
static constexpr uint64_t s_nRdFrameSnapshotGenerationMax = ( uint64_t( 1u ) << 61u ) - 1u;

EA_FORCE_INLINE constexpr uint32_t t3dRdPackGodotRgba32ForGpu( const uint32_t nRgba32 ) {
	// Godot Color::to_rgba32() returns 0xRRGGBBAA. RD R8G8B8A8 vertex data
	// and GLSL unpackUnorm4x8() consume R from the lowest-address/lowest-order byte.
	return ( ( nRgba32 & 0xFF000000u ) >> 24u ) | ( ( nRgba32 & 0x00FF0000u ) >> 8u ) |
			( ( nRgba32 & 0x0000FF00u ) << 8u ) | ( ( nRgba32 & 0x000000FFu ) << 24u );
}

EA_FORCE_INLINE constexpr uint64_t
t3dRdMakeFrameSnapshotToken( const uint32_t nIndex, const uint64_t nGeneration, const bool bPublished ) {
	return ( ( nGeneration & s_nRdFrameSnapshotGenerationMax ) << 2u ) |
			( ( uint64_t )( nIndex ) & s_nRdFrameSnapshotTokenIndexMask ) |
			( bPublished ? s_nRdFrameSnapshotTokenPublishedMask : uint64_t( 0u ) );
}

EA_FORCE_INLINE constexpr uint32_t t3dRdFrameSnapshotTokenIndex( const uint64_t nToken ) {
	return ( uint32_t )( nToken & s_nRdFrameSnapshotTokenIndexMask );
}

EA_FORCE_INLINE constexpr uint64_t t3dRdFrameSnapshotTokenGeneration( const uint64_t nToken ) {
	return ( nToken & ~s_nRdFrameSnapshotTokenPublishedMask ) >> 2u;
}

EA_FORCE_INLINE constexpr bool t3dRdFrameSnapshotTokenIsPublished( const uint64_t nToken ) {
	return ( nToken & s_nRdFrameSnapshotTokenPublishedMask ) != 0u;
}

EA_FORCE_INLINE constexpr bool
t3dRdFrameSnapshotGenerationIsNewer( const uint64_t nCandidate, const uint64_t nConsumed ) {
	if ( nCandidate == 0u || nCandidate == nConsumed ) return false;
	if ( nConsumed == 0u ) return true;
	const uint64_t distance = nCandidate > nConsumed ? nCandidate - nConsumed :
			( s_nRdFrameSnapshotGenerationMax - nConsumed ) + nCandidate;
	return distance <= s_nRdFrameSnapshotGenerationMax / 2u;
}

EA_FORCE_INLINE constexpr bool
t3dRdFrameSnapshotGenerationIsStale( const uint64_t nCandidate, const uint64_t nConsumed ) {
	return nCandidate != 0u && nConsumed != 0u && !t3dRdFrameSnapshotGenerationIsNewer( nCandidate, nConsumed );
}

EA_FORCE_INLINE constexpr uint64_t
t3dRdFrameSnapshotGenerationDistance( const uint64_t nCandidate, const uint64_t nConsumed ) {
	if ( !t3dRdFrameSnapshotGenerationIsNewer( nCandidate, nConsumed ) ) return 0u;
	return nCandidate > nConsumed ? nCandidate - nConsumed :
			( s_nRdFrameSnapshotGenerationMax - nConsumed ) + nCandidate;
}

static_assert( s_nRdFrameSnapshotSlotCount >= 3u, "RD frame publication requires at least three slots" );
static_assert( t3dRdFrameSnapshotTokenIndex( t3dRdMakeFrameSnapshotToken( 2u, 17u, true ) ) == 2u,
		"RD frame publication token index changed" );
static_assert( t3dRdFrameSnapshotTokenGeneration( t3dRdMakeFrameSnapshotToken( 2u, 17u, true ) ) == 17u,
		"RD frame publication token generation changed" );
static_assert( t3dRdFrameSnapshotTokenIsPublished( t3dRdMakeFrameSnapshotToken( 2u, 17u, true ) ),
		"RD frame publication token state changed" );

typedef struct t3dRdTerrainBindings_s {
	RID m_hHeightTextureRd;
	RID m_hDerivedTextureRd;
	RID m_hControlTextureRd;
	RID m_hColorTextureRd;
	RID m_hMiddleCompositeTextureRd;
	RID m_hTerrainShadowMaskTextureRd;
	RID m_hMiddleHeightTextureRd;
	RID m_hMiddleHeightRegionMapTextureRd;
	RID m_hFarHeightTextureRd;
	RID m_hFarColorTextureRd;
	RID m_hAlbedoTextureRd;
	RID m_hFarAlbedoTextureRd;
	RID m_hNormalTextureRd;
	RID m_hSurfaceTextureRd;
	RID m_hMaterialDisplacementTextureRd;
	RID m_hDisplacementTextureRd;
	RID m_hWorldNoiseTextureRd;
	RID m_hMacroNoiseTextureRd;
	RID m_hEditorBrushTextureRd;
} t3dRdTerrainBindings_t;

static constexpr uint32_t s_nRdForwardBridgeAbiVersion = 10u;
static constexpr uint32_t s_nRdForwardMaxViews = 2u;

typedef enum t3dRdForwardCallbackPhase_e {
	T3D_RD_FORWARD_PHASE_PREPARE = 0,
	T3D_RD_FORWARD_PHASE_SUBMIT = 1,
} t3dRdForwardCallbackPhase_t;

// Mirrors RenderForwardClustered::PassMode in Godot 4.7.2. This is part of
// the versioned bridge ABI so F2 can build pipelines with the exact native
// attachment/output contract instead of guessing from pass kind alone.
typedef enum t3dRdForwardNativePassMode_e {
	T3D_RD_FORWARD_NATIVE_PASS_COLOR = 0,
	T3D_RD_FORWARD_NATIVE_PASS_SHADOW = 1,
	T3D_RD_FORWARD_NATIVE_PASS_SHADOW_DP = 2,
	T3D_RD_FORWARD_NATIVE_PASS_DEPTH = 3,
	T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS = 4,
	T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI = 5,
	T3D_RD_FORWARD_NATIVE_PASS_DEPTH_MATERIAL = 6,
	T3D_RD_FORWARD_NATIVE_PASS_SDF = 7,
} t3dRdForwardNativePassMode_t;

typedef enum t3dRdForwardShaderVariant_e {
	T3D_RD_FORWARD_SHADER_COLOR = 0,
	T3D_RD_FORWARD_SHADER_COLOR_SPECULAR,
	T3D_RD_FORWARD_SHADER_COLOR_MOTION,
	T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION,
	T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY,
	T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY,
	T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY,
	T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY,
	T3D_RD_FORWARD_SHADER_DEPTH,
	T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS,
	T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI,
	T3D_RD_FORWARD_SHADER_VARIANT_MAX,
} t3dRdForwardShaderVariant_t;

typedef enum t3dRdForwardEnvironmentFlag_e : uint32_t {
	T3D_RD_FORWARD_ENV_USE_AMBIENT_LIGHT = 1u << 0u,
	T3D_RD_FORWARD_ENV_USE_AMBIENT_CUBEMAP = 1u << 1u,
	T3D_RD_FORWARD_ENV_USE_REFLECTION_CUBEMAP = 1u << 2u,
	T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY = 1u << 3u,
} t3dRdForwardEnvironmentFlag_t;

typedef enum t3dRdForwardPassKind_e {
	T3D_RD_FORWARD_PASS_INVALID = 0,
	T3D_RD_FORWARD_PASS_DEPTH = 1,
	T3D_RD_FORWARD_PASS_OPAQUE = 2,
	T3D_RD_FORWARD_PASS_SHADOW = 3,
	T3D_RD_FORWARD_PASS_MOTION = 4,
	T3D_RD_FORWARD_PASS_MATERIAL = 5,
	T3D_RD_FORWARD_PASS_SDF = 6,
	T3D_RD_FORWARD_PASS_GI = 7,
	T3D_RD_FORWARD_PASS_REFLECTION = 8,
	T3D_RD_FORWARD_PASS_TRANSPARENT = 9,
	T3D_RD_FORWARD_PASS_MAX,
} t3dRdForwardPassKind_t;

// Borrowed Forward+ state is valid only for the synchronous bridge callback.
// Terrain3D owns none of the native RIDs in this structure.
typedef struct t3dRdForwardPassContext_s {
	uint32_t m_nBridgeAbiVersion = s_nRdForwardBridgeAbiVersion;
	uint64_t m_nFrameGeneration = 0u;
	uint32_t m_nResourceGeneration = 0u;
	uint64_t m_nPreparedSerial = 0u;
	t3dRdForwardCallbackPhase_t m_phase = T3D_RD_FORWARD_PHASE_PREPARE;
	t3dRdForwardPassKind_t m_passKind = T3D_RD_FORWARD_PASS_INVALID;
	t3dRdForwardNativePassMode_t m_nativePassMode = T3D_RD_FORWARD_NATIVE_PASS_COLOR;
	int64_t m_nDrawList = -1;
	int64_t m_nFramebufferFormat = -1;
	uint32_t m_nViewCount = 1u;
	uint64_t m_nScenePass = 0u;
	uint64_t m_nRenderFrame = 0u;
	uint64_t m_nViewportKey = 0u;
	uint32_t m_nSampleCount = 1u;
	Rect2i m_renderRegion;
	Vector2i m_renderSize;
	uint32_t m_nColorPassFlags = 0u;
	uint32_t m_nPassFlags = 0u;
	bool m_bDepthPrepassActive = false;
	bool m_bReverseCull = false;
	bool m_bReflectionProbeRender = false;
	Transform3D m_cameraTransform;
	Transform3D m_previousCameraTransform;
	Projection m_szViewProjection[s_nRdForwardMaxViews];
	Projection m_szPreviousViewProjection[s_nRdForwardMaxViews];
	Vector3 m_szEyeOffset[s_nRdForwardMaxViews];
	Vector2 m_taaJitter;
	Vector2 m_previousTaaJitter;
	bool m_bMotionVectorsEnabled = false;
	bool m_bTemporalHistoryValid = false;
	bool m_bViewportReconfigured = false;
	uint32_t m_nCameraVisibleLayers = UINT32_MAX;
	RID m_hShadowLightInstance;
	uint32_t m_nShadowLightType = UINT32_MAX;
	int32_t m_nShadowPassIndex = -1;
	uint32_t m_nShadowCasterMask = UINT32_MAX;
	uint32_t m_nShadowLightCullMask = UINT32_MAX;
	float m_fShadowBias = 0.0f;
	float m_fShadowNormalBias = 0.0f;
	float m_fShadowBiasScale = 1.0f;
	float m_fShadowRangeBegin = 0.0f;
	float m_fShadowRange = 0.0f;
	Rect2 m_shadowAtlasRect;
	Vector2i m_shadowAtlasSize;
	bool m_bShadowPancake = false;
	bool m_bShadowDualParaboloid = false;
	bool m_bShadowDualParaboloidFlip = false;
	uint32_t m_nClusterSize = 0u;
	uint32_t m_nClusterMaxElements = 0u;
	uint32_t m_nClusterShift = 0u;
	uint32_t m_nClusterWidth = 0u;
	uint32_t m_nClusterTypeSize = 0u;
	uint32_t m_nClusterMaxElementsDiv32 = 0u;
	uint32_t m_nDirectionalLightCount = 0u;
	bool m_bDirectionalLightSoftShadows = false;
	float m_fCameraZNear = 0.0f;
	float m_fCameraZFar = 0.0f;
	float m_fReflectionAtlasBorderSize = 0.0f;
	uint32_t m_nEnvironmentFlags = 0u;
	float m_szAmbientLightColorEnergy[4]{};
	float m_fAmbientColorSkyMix = 0.0f;
	float m_fEmissiveExposureNormalization = 1.0f;
	float m_fIblExposureNormalization = 1.0f;
	float m_fRadiancePixelSize = 0.0f;
	float m_fRadianceBorderSize = 0.0f;
	float m_szRadianceInverseXform[12]{};
	RID m_hNativeFramebuffer;
	RID m_hDirectionalLightBuffer;
	RID m_hOmniLightBuffer;
	RID m_hSpotLightBuffer;
	RID m_hAreaLightBuffer;
	RID m_hClusterBuffer;
	RID m_hShadowAtlasTexture;
	RID m_hDirectionalShadowAtlasTexture;
	RID m_hNativeShadowSampler;
	RID m_hLightmapBuffer;
	RID m_hLightmapCaptureBuffer;
	RID m_hReflectionProbeBuffer;
	RID m_hReflectionAtlasTexture;
	RID m_hRadianceTexture;
	RID m_hDecalBuffer;
	RID m_hDecalAtlasTexture;
	RID m_hDecalAtlasSrgbTexture;
	RID m_hAreaLightAtlasTexture;
	RID m_hBaseUniformSet;
	RID m_hRenderPassUniformSet;
} t3dRdForwardPassContext_t;

typedef enum t3dRdDrawGroup_e {
	T3D_RD_DRAW_GROUP_LOD0_NEAR = 0,
	T3D_RD_DRAW_GROUP_OUTER_NEAR = 1,
	T3D_RD_DRAW_GROUP_OUTER_MIDDLE = 2,
	T3D_RD_DRAW_GROUP_OUTER_FAR = 3,
} t3dRdDrawGroup_t;

typedef struct t3dRdClipmapVertex_s {
	int16_t m_nGridX = 0;
	int16_t m_nGridZ = 0;
	uint16_t m_nPartCode = 0u;
	uint16_t m_nFlags = 0u;
} t3dRdClipmapVertex_t;
static_assert( sizeof( t3dRdClipmapVertex_t ) == 8u, "RD clipmap vertex ABI changed" );

typedef struct t3dRdPatch_s {
	float m_fOriginX = 0.0f;
	float m_fOriginZ = 0.0f;
	float m_fScale = 1.0f;
	uint32_t m_nLod = 0u;
	uint32_t m_nTier = 0u;
	uint32_t m_nStitchX = 1u;
	uint32_t m_nStitchZ = 1u;
	uint32_t m_nFlags = 0u;
	float m_fBoundsMinY = 0.0f;
	float m_fBoundsMaxY = 0.0f;
	float m_fBoundsHalfExtent = 0.0f;
	float m_fPreviousOriginX = 0.0f;
	float m_fPreviousOriginZ = 0.0f;
	float m_fPreviousScale = 1.0f;
	float m_fPadding0 = 0.0f;
	float m_fPadding1 = 0.0f;
} t3dRdPatch_t;
static_assert( sizeof( t3dRdPatch_t ) == 64u, "RD patch ABI changed" );

typedef enum t3dRdMaterialFeatureBits_e {
	T3D_RD_MATERIAL_FEATURE_AUTO_SHADER = 1u << 0u,
	T3D_RD_MATERIAL_FEATURE_DUAL_SCALING = 1u << 1u,
	T3D_RD_MATERIAL_FEATURE_MACRO_VARIATION = 1u << 2u,
	T3D_RD_MATERIAL_FEATURE_CONTROL_IMPLICIT = 1u << 3u,
	T3D_RD_MATERIAL_FEATURE_COLOR_IMPLICIT = 1u << 4u,
	T3D_RD_MATERIAL_FEATURE_WORLD_NOISE = 1u << 5u,
	T3D_RD_MATERIAL_FEATURE_DISPLACEMENT = 1u << 6u,
	T3D_RD_MATERIAL_FEATURE_DERIVED_MAPS = 1u << 7u,
	T3D_RD_MATERIAL_FEATURE_MIDDLE_COMPOSITE = 1u << 8u,
	T3D_RD_MATERIAL_FEATURE_MIDDLE_HEIGHT = 1u << 9u,
	T3D_RD_MATERIAL_FEATURE_FAR_ATLAS = 1u << 10u,
	T3D_RD_MATERIAL_FEATURE_FAR_ALBEDO = 1u << 11u,
	T3D_RD_MATERIAL_FEATURE_WORLD_NOISE_CACHE = 1u << 12u,
	T3D_RD_MATERIAL_FEATURE_MACRO_NOISE = 1u << 13u,
} t3dRdMaterialFeatureBits_t;

typedef enum t3dRdMaterialOutputBits_e {
	T3D_RD_MATERIAL_OUTPUT_ALBEDO = 1u << 0u,
	T3D_RD_MATERIAL_OUTPUT_ROUGHNESS = 1u << 1u,
	T3D_RD_MATERIAL_OUTPUT_SPECULAR = 1u << 2u,
	T3D_RD_MATERIAL_OUTPUT_NORMAL = 1u << 3u,
	T3D_RD_MATERIAL_OUTPUT_AO = 1u << 4u,
} t3dRdMaterialOutputBits_t;

typedef enum t3dRdMaterialDebugBits_e {
	T3D_RD_DEBUG_REGION_GRID = 1u << 0u,
	T3D_RD_DEBUG_INSTANCER_GRID = 1u << 1u,
	T3D_RD_DEBUG_VERTEX_GRID = 1u << 2u,
	T3D_RD_DEBUG_CONTOURS = 1u << 3u,
	T3D_RD_DEBUG_NAVIGATION = 1u << 4u,
	T3D_RD_DEBUG_CHECKERED = 1u << 5u,
	T3D_RD_DEBUG_GREY = 1u << 6u,
	T3D_RD_DEBUG_HEIGHTMAP = 1u << 7u,
	T3D_RD_DEBUG_JAGGEDNESS = 1u << 8u,
	T3D_RD_DEBUG_AUTOSHADER = 1u << 9u,
	T3D_RD_DEBUG_CONTROL_TEXTURE = 1u << 10u,
	T3D_RD_DEBUG_CONTROL_BLEND = 1u << 11u,
	T3D_RD_DEBUG_CONTROL_ANGLE = 1u << 12u,
	T3D_RD_DEBUG_CONTROL_SCALE = 1u << 13u,
	T3D_RD_DEBUG_COLORMAP = 1u << 14u,
	T3D_RD_DEBUG_ROUGHMAP = 1u << 15u,
	T3D_RD_DEBUG_DISPLACEMENT = 1u << 16u,
	T3D_RD_DEBUG_TEX_ALBEDO = 1u << 17u,
	T3D_RD_DEBUG_TEX_HEIGHT = 1u << 18u,
	T3D_RD_DEBUG_TEX_NORMAL = 1u << 19u,
	T3D_RD_DEBUG_TEX_ROUGH = 1u << 20u,
	T3D_RD_DEBUG_TEX_AO = 1u << 21u,
} t3dRdMaterialDebugBits_t;

typedef enum t3dRdMaterialLayerBits_e {
	T3D_RD_MATERIAL_LAYER_PACKED_SURFACE = 1u << 0u,
	T3D_RD_MATERIAL_LAYER_RESIDENT = 1u << 1u,
} t3dRdMaterialLayerBits_t;

typedef struct t3dRdMaterialLayer_s {
	float m_fColorR = 1.0f;
	float m_fColorG = 1.0f;
	float m_fColorB = 1.0f;
	float m_fColorA = 1.0f;
	float m_fUvScale = 0.1f;
	float m_fNormalDepth = 1.0f;
	float m_fRoughness = 1.0f;
	float m_fMetallic = 0.0f;
	float m_fAoStrength = 1.0f;
	float m_fAoLightAffect = 0.0f;
	float m_fDetileRotation = 0.0f;
	float m_fDetileShift = 0.0f;
	float m_fDisplacementOffset = 0.0f;
	float m_fDisplacementScale = 0.0f;
	uint32_t m_nPhysicalLayer = 0u;
	uint32_t m_nFlags = 0u;
} t3dRdMaterialLayer_t;
static_assert( sizeof( t3dRdMaterialLayer_t ) == 64u, "RD material layer ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fColorR ) == 0u, "RD material color ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fUvScale ) == 16u, "RD material PBR0 ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fAoStrength ) == 32u, "RD material PBR1 ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fDisplacementOffset ) == 48u, "RD material displacement ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_nPhysicalLayer ) == 56u, "RD material physical-layer ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_nFlags ) == 60u, "RD material flags ABI changed" );

typedef struct alignas( 16 ) t3dRdTierMaterialConfig_s {
	uint32_t m_nSamplingMode = 3u;
	uint32_t m_nNormalMode = 3u;
	uint32_t m_nColorMode = 2u;
	uint32_t m_nSurfaceResponse = 1u;
	uint32_t m_nTextureSurfaceMode = 0u;
	uint32_t m_nLightingModel = 3u;
	uint32_t m_nDetilingMode = 1u;
	uint32_t m_nFiltering = 1u;
	float m_fAuthoredColorContribution = 1.0f;
	float m_fWetnessContribution = 1.0f;
	float m_fMaterialLodBias = 0.0f;
	float m_fQualityLodOffset = 0.0f;
} t3dRdTierMaterialConfig_t;
static_assert( sizeof( t3dRdTierMaterialConfig_t ) == 48u, "RD tier material ABI changed" );
static_assert( offsetof( t3dRdTierMaterialConfig_t, m_nTextureSurfaceMode ) == 16u, "RD tier flags ABI changed" );
static_assert( offsetof( t3dRdTierMaterialConfig_t, m_fAuthoredColorContribution ) == 32u, "RD tier scalar ABI changed" );

typedef struct alignas( 16 ) t3dRdMaterialConfig_s {
	uint32_t m_nFeatureFlags = 0u;
	uint32_t m_nDebugFlags = 0u;
	uint32_t m_nOutputFlags = T3D_RD_MATERIAL_OUTPUT_ALBEDO | T3D_RD_MATERIAL_OUTPUT_ROUGHNESS |
			T3D_RD_MATERIAL_OUTPUT_SPECULAR | T3D_RD_MATERIAL_OUTPUT_NORMAL | T3D_RD_MATERIAL_OUTPUT_AO;
	uint32_t m_nBackgroundMode = 1u;
	uint32_t m_nProjectionMode = 0u;
	uint32_t m_nTessellationMaterialMode = 0u;
	uint32_t m_nWorldNoiseMode = 0u;
	uint32_t m_nTextureLayerCounts = 0u; // 8 bits each: albedo, normal, surface, displacement.
	float m_fDisplacementSharpness = 0.5f;
	float m_fBlendSharpness = 0.5f;
	float m_fDistantNormalScale = 2.0f;
	float m_fGroundLevel = 0.0f;
	float m_fRegionBlend = 0.25f;
	float m_fBiasDistance = 512.0f;
	float m_fMipmapBias = 1.0f;
	float m_fDepthBlur = 0.0f;
	float m_fAutoSlope = 1.0f;
	float m_fAutoHeightReduction = 0.1f;
	uint32_t m_nAutoBaseTexture = 0u;
	uint32_t m_nAutoOverlayTexture = 1u;
	uint32_t m_nDualScaleTexture = 0u;
	uint32_t m_nFarAlbedoLayerCount = 0u;
	float m_fDualScaleReduction = 0.3f;
	float m_fTriScaleReduction = 0.3f;
	float m_fDualScaleFar = 170.0f;
	float m_fDualScaleNear = 100.0f;
	float m_fNoise1Scale = 0.04f;
	float m_fNoise2Scale = 0.076f;
	float m_fNoise1RotationX = 1.0f;
	float m_fNoise1RotationY = 0.0f;
	float m_fMacroVariation1R = 1.0f;
	float m_fMacroVariation1G = 1.0f;
	float m_fMacroVariation1B = 1.0f;
	float m_fMacroVariationSlope = 0.333f;
	float m_fMacroVariation2R = 1.0f;
	float m_fMacroVariation2G = 1.0f;
	float m_fMacroVariation2B = 1.0f;
	float m_fMacroVariationPadding = 0.0f;
	float m_fNoise1OffsetX = 0.5f;
	float m_fNoise1OffsetY = 0.5f;
	float m_fWorldNoiseCachePeriod = 64.0f;
	float m_fWorldNoiseLodDistance = 7500.0f;
	float m_fWorldNoiseScale = 5.0f;
	float m_fWorldNoiseHeight = 32.0f;
	uint32_t m_nWorldNoiseMaxOctaves = 4u;
	uint32_t m_nWorldNoiseMinOctaves = 2u;
	uint32_t m_nWorldNoiseCacheEnabled = 1u;
	uint32_t m_nWorldNoiseFragmentNormals = 0u;
	float m_fWorldNoiseOffsetX = 0.0f;
	float m_fWorldNoiseOffsetY = 0.0f;
	float m_fWorldNoiseOffsetZ = 0.0f;
	float m_fWorldNoiseCacheMaxLod = 0.0f;
	float m_fSilhouetteR = 0.04f;
	float m_fSilhouetteG = 0.04f;
	float m_fSilhouetteB = 0.04f;
	float m_fSilhouetteA = 1.0f;
	float m_fContourStep = 1.0f;
	float m_fEditorOverlayOpacity = 0.55f;
	float m_fReserved2 = 0.0f;
	float m_fReserved3 = 0.0f;
	t3dRdTierMaterialConfig_t m_szTier[3]{};
} t3dRdMaterialConfig_t;
static_assert( sizeof( t3dRdMaterialConfig_t ) == 384u, "RD material config ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_fDisplacementSharpness ) == 32u, "RD material general ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_fAutoSlope ) == 64u, "RD material auto ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_nDualScaleTexture ) == 80u, "RD material dual ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_fMacroVariation1R ) == 120u, "RD material macro ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_fNoise1OffsetX ) == 152u, "RD material noise ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_fWorldNoiseScale ) == 168u, "RD material world-noise ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_fWorldNoiseOffsetX ) == 192u, "RD material world offset ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_fSilhouetteR ) == 208u, "RD material silhouette ABI changed" );
static_assert( offsetof( t3dRdMaterialConfig_t, m_szTier ) == 240u, "RD material tier-table ABI changed" );

typedef struct alignas( 16 ) t3dRdEditorDecal_s {
	float m_fPositionX = 0.0f;
	float m_fPositionY = 0.0f;
	float m_fSize = 0.0f;
	float m_fRotation = 0.0f;
	float m_fColorR = 0.0f;
	float m_fColorG = 0.0f;
	float m_fColorB = 0.0f;
	float m_fColorA = 0.0f;
	uint32_t m_nVisible = 0u;
	uint32_t m_nPartFlags = 0u;
	uint32_t m_nPadding0 = 0u;
	uint32_t m_nPadding1 = 0u;
} t3dRdEditorDecal_t;
static_assert( sizeof( t3dRdEditorDecal_t ) == 48u, "RD editor decal ABI changed" );
static_assert( offsetof( t3dRdEditorDecal_t, m_fColorR ) == 16u, "RD editor decal color ABI changed" );
static_assert( offsetof( t3dRdEditorDecal_t, m_nVisible ) == 32u, "RD editor decal flags ABI changed" );

typedef struct alignas( 16 ) t3dRdEditorOverlayConfig_s {
	t3dRdEditorDecal_t m_szDecals[3]{};
	float m_fHeightmapBlackHeight = -100.0f;
	float m_fHeightmapWhiteHeight = 300.0f;
	float m_fContourInterval = 1.0f;
	float m_fContourThickness = 1.0f;
	float m_fContourColorR = 0.85f;
	float m_fContourColorG = 0.85f;
	float m_fContourColorB = 0.19f;
	float m_fContourColorA = 1.0f;
	float m_fInstancerCellSize = 32.0f;
	float m_fPadding0 = 0.0f;
	float m_fPadding1 = 0.0f;
	float m_fPadding2 = 0.0f;
} t3dRdEditorOverlayConfig_t;
static_assert( sizeof( t3dRdEditorOverlayConfig_t ) == 192u, "RD editor overlay ABI changed" );
static_assert( offsetof( t3dRdEditorOverlayConfig_t, m_fHeightmapBlackHeight ) == 144u, "RD editor debug ABI changed" );
static_assert( offsetof( t3dRdEditorOverlayConfig_t, m_fContourColorR ) == 160u, "RD editor contour ABI changed" );
static_assert( offsetof( t3dRdEditorOverlayConfig_t, m_fInstancerCellSize ) == 176u, "RD editor grid ABI changed" );


// Batch D regular-instance ABI. Mesh handles are generation-safe: a handle is
// only valid while its generation matches the owning Terrain3DRdMeshCache.
typedef struct t3dRdMeshHandle_s {
	uint32_t m_nSlot = UINT32_MAX;
	uint32_t m_nGeneration = 0u;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_nSlot != UINT32_MAX && m_nGeneration != 0u;
	}
	EA_FORCE_INLINE bool operator==( const t3dRdMeshHandle_s &rOther ) const {
		return m_nSlot == rOther.m_nSlot && m_nGeneration == rOther.m_nGeneration;
	}
} t3dRdMeshHandle_t;
static_assert( sizeof( t3dRdMeshHandle_t ) == 8u, "RD mesh handle ABI changed" );

typedef struct t3dRdSurfaceHandle_s {
	uint32_t m_nSlot = UINT32_MAX;
	uint32_t m_nGeneration = 0u;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_nSlot != UINT32_MAX && m_nGeneration != 0u;
	}
} t3dRdSurfaceHandle_t;
static_assert( sizeof( t3dRdSurfaceHandle_t ) == 8u, "RD surface handle ABI changed" );

// One interleaved static-mesh vertex. Normals/tangents are signed normalized
// 16-bit components and UVs are binary16. This keeps imported geometry at
// 48 bytes/vertex while preserving the attributes used by Terrain3D assets.
typedef struct alignas( 16 ) t3dRdMeshVertex_s {
	float m_szPosition[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t m_nColorRgba8 = 0xFFFFFFFFu;
	int16_t m_szNormal[4] = { 0, 32767, 0, 0 };
	int16_t m_szTangent[4] = { 32767, 0, 0, 32767 };
	uint16_t m_szUv[2] = { 0u, 0u };
	uint16_t m_szUv2[2] = { 0u, 0u };
	uint32_t m_nPadding0 = 0u;
	uint32_t m_nPadding1 = 0u;
} t3dRdMeshVertex_t;
static_assert( sizeof( t3dRdMeshVertex_t ) == 48u, "RD mesh vertex ABI changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_nColorRgba8 ) == 12u, "RD mesh color offset changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_szNormal ) == 16u, "RD mesh normal offset changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_szTangent ) == 24u, "RD mesh tangent offset changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_szUv ) == 32u, "RD mesh UV offset changed" );

// Production static-instance record: 32 bytes. Position stays float32; rotation
// is a normalized quaternion packed as snorm16x4; scale is binary16x3; color is
// RGBA8. Mesh/material/region state is cell-level and is not redundantly stored
// per instance.
typedef struct alignas( 16 ) t3dRdInstanceRecord_s {
	float m_szPosition[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t m_nQuaternion01 = 0u;
	uint32_t m_nQuaternion23 = 0u;
	uint32_t m_nScale01 = 0u;
	uint32_t m_nScale2Flags = 0u;
	uint32_t m_nColorRgba8 = 0xFFFFFFFFu;
} t3dRdInstanceRecord_t;
static_assert( sizeof( t3dRdInstanceRecord_t ) == 32u, "RD instance record ABI changed" );
static_assert( offsetof( t3dRdInstanceRecord_t, m_nQuaternion01 ) == 12u, "RD instance rotation offset changed" );
static_assert( offsetof( t3dRdInstanceRecord_t, m_nColorRgba8 ) == 28u, "RD instance color offset changed" );

typedef enum t3dRdInstanceFlags_e : uint32_t {
	T3D_RD_INSTANCE_NONE = 0u,
	T3D_RD_INSTANCE_CAST_SHADOWS = 1u << 0u,
	T3D_RD_INSTANCE_DOUBLE_SIDED = 1u << 1u,
	T3D_RD_INSTANCE_SHADOWS_ONLY = 1u << 2u,
	T3D_RD_INSTANCE_HIGHLIGHTED = 1u << 3u,
	T3D_RD_INSTANCE_CULL_FRONT = 1u << 4u,
	T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED = 1u << 5u,
	T3D_RD_INSTANCE_ALPHA_BLEND = 1u << 6u,
	T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS = 1u << 7u,
	T3D_RD_INSTANCE_FADE_ACTIVE = 1u << 24u,
	T3D_RD_INSTANCE_HLOD = 1u << 25u,
	T3D_RD_INSTANCE_FADE_SHIFT = 8u,
	T3D_RD_INSTANCE_FADE_MASK = 0x00FFFF00u,
	T3D_RD_INSTANCE_DEPTH_PREPASS_PASS = 1u << 31u,
} t3dRdInstanceFlags_t;

// CPU publication descriptor for one logical resident cell/LOD. The renderer
// uses the AABB for coarse rejection, then writes visible-instance indices into
// a single persistent indirection buffer grouped by mesh surface/material.
EA_FORCE_INLINE constexpr uint64_t t3dRdRegularMeshSourceKey( const uint16_t nAssetId, const int16_t nLod ) {
	return 0x1000000000000000ull | ( uint64_t )( nAssetId ) | ( ( uint64_t )( ( uint16_t )( nLod ) ) << 16u );
}

EA_FORCE_INLINE constexpr uint64_t t3dRdHlodMeshSourceKey( const uint32_t nEntryIndex ) {
	return 0x2000000000000000ull | ( uint64_t )( nEntryIndex + 1u );
}

typedef struct t3dRdInstanceCell_s {
	uint64_t m_nMeshSourceKey = 0u;
	int32_t m_nRegionX = 0;
	int32_t m_nRegionZ = 0;
	int16_t m_nCellX = 0;
	int16_t m_nCellZ = 0;
	uint16_t m_nMeshAssetId = 0u;
	int16_t m_nLod = 0;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nRevision = 0u;
	uint32_t m_nSettingsRevision = 0u;
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nFlags = 0u;
	float m_fLodBegin = 0.0f;
	float m_fLodEnd = 0.0f;
	float m_fFadeMargin = 0.0f;
	float m_fPadding0 = 0.0f;
	AABB m_bounds;
} t3dRdInstanceCell_t;


// Batch E HLOD culling descriptor. One descriptor is emitted per HLOD draw
// packet because each surface owns a distinct indirect command while sharing
// the package transform/state record. The compute cull pass only mutates the
// command instance count; geometry/material ownership stays in the shared mesh
// cache and instance renderer.
typedef struct alignas( 16 ) t3dRdHlodCullDescriptor_s {
	float m_szBoundsCenter[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float m_szBoundsExtent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nCommandIndex = 0u;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nReserved0 = 0u;
} t3dRdHlodCullDescriptor_t;
static_assert( sizeof( t3dRdHlodCullDescriptor_t ) == 48u, "RD HLOD descriptor ABI changed" );
static_assert( offsetof( t3dRdHlodCullDescriptor_t, m_nVisibilityLayers ) == 32u, "RD HLOD descriptor flags ABI changed" );

// Material subset extracted from imported BaseMaterial3D resources. Texture
// object IDs are immutable publication keys resolved by the direct-RD mesh
// cache into Terrain3D-owned texture RIDs without native material proxies.
typedef enum t3dRdInstanceMaterialFlags_e : uint32_t {
	T3D_RD_INSTANCE_MATERIAL_NONE = 0u,
	T3D_RD_INSTANCE_MATERIAL_ALPHA_SCISSOR = 1u << 0u,
	T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED = 1u << 1u,
	T3D_RD_INSTANCE_MATERIAL_HAS_ALBEDO_TEXTURE = 1u << 2u,
	T3D_RD_INSTANCE_MATERIAL_HAS_NORMAL_TEXTURE = 1u << 3u,
	T3D_RD_INSTANCE_MATERIAL_HAS_ORM_TEXTURE = 1u << 4u,
	T3D_RD_INSTANCE_MATERIAL_EMISSIVE = 1u << 5u,
	T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT = 1u << 6u,
	T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR = 1u << 7u,
	T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR_SRGB = 1u << 8u,
	T3D_RD_INSTANCE_MATERIAL_CULL_FRONT = 1u << 9u,
	T3D_RD_INSTANCE_MATERIAL_NORMAL_MAPPING = 1u << 10u,
	T3D_RD_INSTANCE_MATERIAL_AO_ENABLED = 1u << 11u,
	T3D_RD_INSTANCE_MATERIAL_UNSHADED = 1u << 12u,
	T3D_RD_INSTANCE_MATERIAL_DONT_RECEIVE_SHADOWS = 1u << 13u,
	T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND = 1u << 14u,
	T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS = 1u << 15u,
	T3D_RD_INSTANCE_MATERIAL_FILTER_SHIFT = 16u,
	T3D_RD_INSTANCE_MATERIAL_FILTER_MASK = 7u << T3D_RD_INSTANCE_MATERIAL_FILTER_SHIFT,
	T3D_RD_INSTANCE_MATERIAL_ROUGHNESS_TEXTURE = 1u << 19u,
} t3dRdInstanceMaterialFlags_t;

typedef struct alignas( 16 ) t3dRdInstanceMaterial_s {
	float m_szAlbedo[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float m_fMetallic = 0.0f;
	float m_fRoughness = 1.0f;
	float m_fAo = 1.0f;
	float m_fAlphaScissor = 0.5f;
	float m_szEmission[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t m_nFlags = 0u;
	float m_fNormalScale = 1.0f;
	uint32_t m_nReserved0 = 0u;
	uint32_t m_nAlbedoTextureIdLow = 0u;
	uint32_t m_nAlbedoTextureIdHigh = 0u;
	uint32_t m_nNormalTextureIdLow = 0u;
	uint32_t m_nNormalTextureIdHigh = 0u;
	uint32_t m_nOrmTextureIdLow = 0u;
	uint32_t m_nOrmTextureIdHigh = 0u;
} t3dRdInstanceMaterial_t;
static_assert( sizeof( t3dRdInstanceMaterial_t ) == 80u, "RD instance material ABI changed" );
static_assert( offsetof( t3dRdInstanceMaterial_t, m_fNormalScale ) == 48u, "RD instance normal-scale ABI changed" );
static_assert( offsetof( t3dRdInstanceMaterial_t, m_nAlbedoTextureIdLow ) == 56u, "RD instance texture ABI changed" );


typedef struct alignas( 16 ) t3dRdOceanState_s {
	uint32_t m_nEnabled = 0u;
	uint32_t m_nPatchCount = 0u;
	uint32_t m_nRenderLayers = 1u;
	uint32_t m_nCastShadows = 0u;
	uint32_t m_nVertexIterations = 1u;
	uint32_t m_nFragmentIterations = 3u;
	uint32_t m_nRefractionMode = 1u;
	uint32_t m_nFoamMode = 1u;
	uint32_t m_nScatteringMode = 1u;
	uint32_t m_nShadingMode = 1u;
	uint32_t m_nReserved0 = 0u;
	uint32_t m_nReserved1 = 0u;
	float m_fTimeSeconds = 0.0f;
	float m_fMeshSize = 32.0f;
	float m_fVertexSpacing = 4.0f;
	float m_fSubdiv = 1.0f;
	float m_fTargetX = 0.0f;
	float m_fTargetY = 0.0f;
	float m_fTargetZ = 0.0f;
	float m_fSeaLevel = 5.0f;
	float m_szWaterColor[4] = { 0.1058f, 0.1568f, 0.2117f, 1.0f };
	float m_fVisibleDepth = 256.0f;
	float m_fDepthCurve = 0.1f;
	float m_fShoreBlend = 1.0f;
	float m_fRefractionStrength = 0.02f;
	float m_fTimeScale = 8.0f;
	float m_fHeightScale = 3.0f;
	float m_fOceanScale = 2.0f;
	float m_fFresnelScale = 0.5f;
	float m_fFresnelPower = 1.0f;
	float m_fSpecular = 1.0f;
	float m_fFoamHeight = 1.35f;
	float m_fFoamTopIntensity = 2.0f;
	float m_fFoamDistance = 3072.0f;
	float m_fFoamShoreIntensity = 1.3f;
	float m_fFoamNoiseScale = 1.5f;
	float m_fHeightScattering = 6.0f;
	float m_fHeightThreshold = 0.1f;
	float m_fHeightAngleThreshold = 0.21f;
	float m_fFresnelScattering = 20.0f;
	float m_fDirectScattering = 10.0f;
	float m_fCullMargin = 0.0f;
	float m_fReserved2 = 0.0f;
	uint64_t m_nGeneration = 0u;
	t3dRdPatch_t m_szPatches[s_nMaxClipmapLevels]{};
} t3dRdOceanState_t;

typedef struct t3dRdFrameSnapshot_s {
	uint64_t m_nPublicationGeneration = 0u;
	uint64_t m_nGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	uint64_t m_nRegionCacheGeneration = 0u;
	uint64_t m_nTerrainTextureGeneration = 0u;
	uint64_t m_nMaterialTextureGeneration = 0u;
	uint32_t m_nPatchCount = 0u;
	uint32_t m_nRegionSize = 128u;
	uint32_t m_nRegionMapSize = 0u;
	uint32_t m_nMeshSize = 48u;
	uint32_t m_nMaterialLayerCount = 1u;
	uint32_t m_nCameraCullMask = UINT32_MAX;
	uint32_t m_nRenderLayers = UINT32_MAX;
	int32_t m_nShadowLodCount = 2;
	int32_t m_nNearShadowLodCount = -1;
	int32_t m_nMiddleShadowLodCount = -1;
	int32_t m_nFarShadowLodCount = -1;
	bool m_bCastShadows = true;
	bool m_bNearShadowEnabled = true;
	bool m_bMiddleShadowEnabled = true;
	bool m_bFarShadowEnabled = true;
	float m_fVertexSpacing = 1.0f;
	float m_fHeightMin = s_fRegionHeightMin;
	float m_fHeightRange = s_fRegionHeightRange;
	float m_fAmbient = 0.28f;
	float m_fFogDensity = 0.0f;
	float m_fFogStart = 0.0f;
	float m_fFogEnd = 1000000.0f;
	float m_fFogCurve = 1.0f;
	uint32_t m_nFogMode = 0u;
	float m_fDisplacementScale = 1.0f;
	bool m_bDisplacementEnabled = false;
	Vector3 m_targetPosition = Vector3();
	float m_fSubdiv = 1.0f;
	Color m_lightColor = Color( 1.0, 1.0, 1.0, 1.0 );
	Vector3 m_lightDirection = Vector3( 0.0, -1.0, 0.0 );
	Color m_fogColor = Color( 0.0, 0.0, 0.0, 1.0 );
	t3dRdPatch_t m_szPatches[s_nMaxClipmapLevels]{};
	t3dRdMaterialLayer_t m_szMaterials[s_nRdMaterialLayerCount]{};
	t3dRdMaterialConfig_t m_materialConfig{};
	t3dRdEditorOverlayConfig_t m_editorOverlayConfig{};
	t3dRdOceanState_t m_ocean{};
} t3dRdFrameSnapshot_t;

typedef struct alignas( 16 ) t3dRdFrameUniforms_s {
	float m_mViewProjection[s_nRdForwardMaxViews][16]{};
	float m_mView[16]{};
	float m_mPreviousViewProjection[s_nRdForwardMaxViews][16]{};
	float m_szMotionJitter[4]{};
	float m_szMotionParams[4]{};
	float m_fLightDirectionX = 0.0f;
	float m_fLightDirectionY = -1.0f;
	float m_fLightDirectionZ = 0.0f;
	float m_fAmbient = 0.28f;
	float m_fLightColorR = 1.0f;
	float m_fLightColorG = 1.0f;
	float m_fLightColorB = 1.0f;
	float m_fLightColorA = 1.0f;
	float m_fRegionSize = 128.0f;
	float m_fRegionMapSize = 0.0f;
	float m_fHeightMin = s_fRegionHeightMin;
	float m_fHeightRange = s_fRegionHeightRange;
	float m_fVertexSpacing = 1.0f;
	float m_fMeshSize = 48.0f;
	float m_fDebugBlend = 0.0f;
	float m_fMaterialLayerCount = 1.0f;
	float m_fTargetX = 0.0f;
	float m_fTargetZ = 0.0f;
	float m_fSubdiv = 1.0f;
	float m_fTargetPadding = 0.0f;
	float m_szCamera[s_nRdForwardMaxViews][4]{};
	float m_fFogR = 0.0f;
	float m_fFogG = 0.0f;
	float m_fFogB = 0.0f;
	float m_fFogDensity = 0.0f;
	float m_fFogStart = 0.0f;
	float m_fFogEnd = 1000000.0f;
	float m_fFogCurve = 1.0f;
	float m_fFogMode = 0.0f;
	float m_fDisplacementScale = 1.0f;
	float m_fDisplacementEnabled = 0.0f;
	float m_fDisplacementPadding0 = 0.0f;
	float m_fDisplacementPadding1 = 0.0f;
} t3dRdFrameUniforms_t;
static_assert( sizeof( t3dRdFrameUniforms_t ) == 512u, "RD frame UBO must match GLSL std140 layout" );

typedef struct t3dRdIndexedIndirectCommand_s {
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nFirstIndex = 0u;
	int32_t m_nVertexOffset = 0;
	uint32_t m_nFirstInstance = 0u;
} t3dRdIndexedIndirectCommand_t;
static_assert( sizeof( t3dRdIndexedIndirectCommand_t ) == 20u, "RD indexed indirect command ABI changed" );

typedef struct t3dRdCullPushConstants_s {
	uint32_t m_nPatchCount = 0u;
	uint32_t m_nLod0IndexCount = 0u;
	uint32_t m_nOuterIndexCount = 0u;
	uint32_t m_nMaxPatches = s_nMaxClipmapLevels;
} t3dRdCullPushConstants_t;
static_assert( sizeof( t3dRdCullPushConstants_t ) == 16u, "RD cull push constants ABI changed" );

typedef struct t3dRdDrawPushConstants_s {
	uint32_t m_nDirectPatchIndex = 0u;
	uint32_t m_nDirectMode = 0u;
	uint32_t m_nPadding0 = 0u;
	uint32_t m_nPadding1 = 0u;
} t3dRdDrawPushConstants_t;
static_assert( sizeof( t3dRdDrawPushConstants_t ) == 16u, "RD draw push constants ABI changed" );

#endif // TERRAIN3D_RD_TYPES_H
