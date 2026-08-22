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

typedef struct t3dRdClipmapVertex_s {
	float m_fX = 0.0f;
	float m_fY = 0.0f;
	float m_fZ = 0.0f;
	float m_fPartCode = 0.0f;
} t3dRdClipmapVertex_t;
static_assert( sizeof( t3dRdClipmapVertex_t ) == 16u, "RD clipmap vertex ABI changed" );

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

typedef struct t3dRdFrameSnapshot_s {
	uint64_t m_nGeneration = 0u;
	uint32_t m_nPatchCount = 0u;
	uint32_t m_nRegionSize = 128u;
	uint32_t m_nRegionMapSize = 0u;
	uint32_t m_nMeshSize = 48u;
	uint32_t m_nMaterialLayerCount = 1u;
	float m_fVertexSpacing = 1.0f;
	float m_fHeightMin = s_fRegionHeightMin;
	float m_fHeightRange = s_fRegionHeightRange;
	float m_fAmbient = 0.28f;
	Vector3 m_targetPosition = Vector3();
	float m_fSubdiv = 1.0f;
	Color m_lightColor = Color( 1.0, 1.0, 1.0, 1.0 );
	Vector3 m_lightDirection = Vector3( 0.0, -1.0, 0.0 );
	t3dRdPatch_t m_szPatches[s_nMaxClipmapLevels]{};
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
} t3dRdFrameUniforms_t;
static_assert( sizeof( t3dRdFrameUniforms_t ) == 144u, "RD frame UBO must match GLSL std140 layout" );

typedef struct t3dRdDrawPushConstants_s {
	float m_fOriginX = 0.0f;
	float m_fOriginZ = 0.0f;
	float m_fScale = 1.0f;
	float m_fLod = 0.0f;
	float m_fTier = 0.0f;
	float m_fStitchX = 1.0f;
	float m_fStitchZ = 1.0f;
	float m_fFlags = 0.0f;
} t3dRdDrawPushConstants_t;
static_assert( sizeof( t3dRdDrawPushConstants_t ) == 32u, "RD push constants must match GLSL layout" );

VARIANT_ENUM_CAST( t3dRenderBackend_t );
VARIANT_ENUM_CAST( t3dRdNativeProxyMode_t );

#endif // TERRAIN3D_RD_TYPES_H
