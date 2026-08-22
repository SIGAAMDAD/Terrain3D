// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_TYPES_H
#define TERRAIN3D_RD_TYPES_H

#include "Terrain3DConstants.h"

#include <cstdint>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

static constexpr uint32_t s_nRdMaterialLayerCount = 32u;
static constexpr uint32_t s_nRdDrawGroupCount = 4u;

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
} t3dRdPatch_t;
static_assert( sizeof( t3dRdPatch_t ) == 32u, "RD patch ABI changed" );

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
	float m_fPhysicalLayer = 0.0f;
	float m_fFlags = 0.0f;
} t3dRdMaterialLayer_t;
static_assert( sizeof( t3dRdMaterialLayer_t ) == 64u, "RD material layer ABI changed" );

typedef struct t3dRdFrameSnapshot_s {
	uint64_t m_nGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
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

VARIANT_ENUM_CAST( t3dRenderBackend_t );
VARIANT_ENUM_CAST( t3dRdNativeProxyMode_t );

#endif // TERRAIN3D_RD_TYPES_H
