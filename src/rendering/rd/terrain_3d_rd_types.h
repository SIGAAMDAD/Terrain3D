// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_TYPES_H
#define TERRAIN3D_RD_TYPES_H

#include "Terrain3DConstants.h"

#include <cstddef>
#include <cstdint>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

static constexpr uint32_t s_nRdMaterialLayerCount = 32u;
static constexpr uint32_t s_nRdDrawGroupCount = 4u;
static constexpr uint32_t s_nRdFrameSnapshotSlotCount = 3u;
static constexpr uint32_t s_nRdInvalidFrameSnapshotIndex = s_nRdFrameSnapshotSlotCount;
static constexpr uint64_t s_nRdFrameSnapshotTokenIndexMask = 0x3u;
static constexpr uint64_t s_nRdFrameSnapshotTokenPublishedMask = uint64_t( 1u ) << 63u;
static constexpr uint64_t s_nRdFrameSnapshotGenerationMax = ( uint64_t( 1u ) << 61u ) - 1u;

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

typedef enum t3dRenderBackend_e {
	T3D_RENDER_BACKEND_RENDERING_SERVER = 0,
	T3D_RENDER_BACKEND_RENDERING_DEVICE_DIRECT = 1,
	T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID = 2,
} t3dRenderBackend_t;

typedef enum t3dRdNativeProxyMode_e {
	T3D_RD_NATIVE_PROXY_DISABLED = 0,
	T3D_RD_NATIVE_PROXY_SHADOWS_ONLY = 1,
	T3D_RD_NATIVE_PROXY_FULL = 2,
} t3dRdNativeProxyMode_t;

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
	uint32_t m_nReserved0 = 0u;
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
	uint32_t m_nReserved1 = 0u;
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
	float m_fWorldNoisePadding = 0.0f;
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
} t3dRdFrameSnapshot_t;

typedef struct alignas( 16 ) t3dRdFrameUniforms_s {
	float m_mViewProjection[16]{};
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
	float m_fCameraX = 0.0f;
	float m_fCameraY = 0.0f;
	float m_fCameraZ = 0.0f;
	float m_fCameraPadding = 0.0f;
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
static_assert( sizeof( t3dRdFrameUniforms_t ) == 208u, "RD frame UBO must match GLSL std140 layout" );

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

VARIANT_ENUM_CAST( t3dRenderBackend_t );
VARIANT_ENUM_CAST( t3dRdNativeProxyMode_t );

#endif // TERRAIN3D_RD_TYPES_H
