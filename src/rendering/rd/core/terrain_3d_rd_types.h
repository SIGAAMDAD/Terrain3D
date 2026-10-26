// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_TYPES_H
#define TERRAIN3D_RD_TYPES_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"
#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;


EA_FORCE_INLINE uint32_t t3dRdPackGodotRgba32ForGpu( const uint32_t nRgba32 ) {
	// Godot Color::to_rgba32() returns 0xRRGGBBAA. RD R8G8B8A8 vertex data
	// and GLSL unpackUnorm4x8() consume R from the lowest-address/lowest-order byte.
	return ( ( nRgba32 & 0xFF000000u ) >> 24u ) | ( ( nRgba32 & 0x00FF0000u ) >> 8u ) | ( ( nRgba32 & 0x0000FF00u ) << 8u )
	        | ( ( nRgba32 & 0x000000FFu ) << 24u );
}

EA_FORCE_INLINE uint64_t t3dRdMakeFrameSnapshotToken( const uint32_t nIndex, const uint64_t nGeneration, const bool bPublished ) {
	return ( ( nGeneration & T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX ) << 2u ) | ( ( uint64_t )( nIndex )&T3D_RD_FRAME_SNAPSHOT_TOKEN_INDEX_MASK )
	        | ( bPublished ? T3D_RD_FRAME_SNAPSHOT_TOKEN_PUBLISHED_MASK : uint64_t( 0u ) );
}

EA_FORCE_INLINE uint32_t t3dRdFrameSnapshotTokenIndex( const uint64_t nToken ) {
	return ( uint32_t )( nToken & T3D_RD_FRAME_SNAPSHOT_TOKEN_INDEX_MASK );
}

EA_FORCE_INLINE uint64_t t3dRdFrameSnapshotTokenGeneration( const uint64_t nToken ) {
	return ( nToken & ~T3D_RD_FRAME_SNAPSHOT_TOKEN_PUBLISHED_MASK ) >> 2u;
}

EA_FORCE_INLINE bool t3dRdFrameSnapshotTokenIsPublished( const uint64_t nToken ) {
	return ( nToken & T3D_RD_FRAME_SNAPSHOT_TOKEN_PUBLISHED_MASK ) != 0u;
}

EA_FORCE_INLINE bool t3dRdFrameSnapshotGenerationIsNewer( const uint64_t nCandidate, const uint64_t nConsumed ) {
	if ( nCandidate == 0u || nCandidate == nConsumed ) {
		return false;
	}
	if ( nConsumed == 0u ) {
		return true;
	}
	const uint64_t distance =
	        nCandidate > nConsumed ? nCandidate - nConsumed : ( T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX - nConsumed ) + nCandidate;
	return distance <= T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX / 2u;
}

EA_FORCE_INLINE bool t3dRdFrameSnapshotGenerationIsStale( const uint64_t nCandidate, const uint64_t nConsumed ) {
	return nCandidate != 0u && nConsumed != 0u && !t3dRdFrameSnapshotGenerationIsNewer( nCandidate, nConsumed );
}

EA_FORCE_INLINE uint64_t t3dRdFrameSnapshotGenerationDistance( const uint64_t nCandidate, const uint64_t nConsumed ) {
	if ( !t3dRdFrameSnapshotGenerationIsNewer( nCandidate, nConsumed ) ) {
		return 0u;
	}
	return nCandidate > nConsumed ? nCandidate - nConsumed : ( T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX - nConsumed ) + nCandidate;
}

static_assert( T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT >= 3u, "RD frame publication requires at least three slots" );
static_assert( ( 2u & T3D_RD_FRAME_SNAPSHOT_TOKEN_INDEX_MASK ) == 2u, "RD frame publication token index mask changed" );
static_assert( ( 17u & T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX ) == 17u, "RD frame publication token generation mask changed" );
static_assert( T3D_RD_FRAME_SNAPSHOT_TOKEN_PUBLISHED_MASK != 0u, "RD frame publication token state mask changed" );

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



// t3dRdForwardCallbackPhase_e centralized in Terrain3DTypes.h.


// Mirrors RenderForwardClustered::PassMode in Godot 4.7.2. This is part of
// the versioned bridge ABI so F2 can build pipelines with the exact native
// attachment/output contract instead of guessing from pass kind alone.
// t3dRdForwardNativePassMode_e centralized in Terrain3DTypes.h.


// t3dRdForwardShaderVariant_e centralized in Terrain3DTypes.h.



// t3dRdForwardEnvironmentFlag_e centralized in Terrain3DTypes.h.


// t3dRdForwardPassKind_e centralized in Terrain3DTypes.h.


// Borrowed Forward+ state is valid only for the synchronous bridge callback.
// Terrain3D owns none of the native RIDs in this structure.
typedef struct t3dRdForwardPassContext_s {

	uint32_t m_nBridgeAbiVersion = T3D_RD_FORWARD_BRIDGE_ABI_VERSION;
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
	Projection m_szViewProjection[T3D_RD_FORWARD_MAX_VIEWS];
	Projection m_szPreviousViewProjection[T3D_RD_FORWARD_MAX_VIEWS];
	Vector3 m_szEyeOffset[T3D_RD_FORWARD_MAX_VIEWS];
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
	RID m_hPreviousDepthTexture;
	RID m_hDecalBuffer;
	RID m_hDecalAtlasTexture;
	RID m_hDecalAtlasSrgbTexture;
	RID m_hAreaLightAtlasTexture;
	RID m_hBaseUniformSet;
	RID m_hRenderPassUniformSet;
} t3dRdForwardPassContext_t;


// t3dRdDrawGroup_e centralized in Terrain3DTypes.h.



// t3dRdPatch_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdPatch_t ) == 64u, "RD patch ABI changed" );

// t3dRdMaterialFeatureBits_e centralized in Terrain3DTypes.h.


// t3dRdMaterialOutputBits_e centralized in Terrain3DTypes.h.


// t3dRdMaterialDebugBits_e centralized in Terrain3DTypes.h.


// t3dRdMaterialLayerBits_e centralized in Terrain3DTypes.h.


// t3dRdMaterialLayer_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdMaterialLayer_t ) == 80u, "RD material layer ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fColorR ) == 0u, "RD material color ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fUvScale ) == 16u, "RD material PBR0 ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fAoStrength ) == 32u, "RD material PBR1 ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_fDisplacementOffset ) == 48u, "RD material displacement ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_nPhysicalLayer ) == 56u, "RD material physical-layer ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_nFlags ) == 60u, "RD material flags ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_nMicroVTAllocation ) == 64u, "RD material micro-VT allocation ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_nMicroVTGeneration ) == 68u, "RD material micro-VT generation ABI changed" );
static_assert( offsetof( t3dRdMaterialLayer_t, m_nMicroVTFlags ) == 72u, "RD material micro-VT flags ABI changed" );

// t3dRdTierMaterialConfig_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdTierMaterialConfig_t ) == 48u, "RD tier material ABI changed" );
static_assert( offsetof( t3dRdTierMaterialConfig_t, m_nTextureSurfaceMode ) == 16u, "RD tier flags ABI changed" );
static_assert( offsetof( t3dRdTierMaterialConfig_t, m_fAuthoredColorContribution ) == 32u, "RD tier scalar ABI changed" );

// t3dRdMaterialConfig_s centralized in Terrain3DTypes.h.

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

// t3dRdEditorDecal_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdEditorDecal_t ) == 48u, "RD editor decal ABI changed" );
static_assert( offsetof( t3dRdEditorDecal_t, m_fColorR ) == 16u, "RD editor decal color ABI changed" );
static_assert( offsetof( t3dRdEditorDecal_t, m_nVisible ) == 32u, "RD editor decal flags ABI changed" );

// t3dRdEditorOverlayConfig_s centralized in Terrain3DTypes.h.

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
// 16-bit components and UVs are binary16. Normal W is reserved by P31: zero
// marks the primary winding and +1 marks a cold-generated backside companion.
// The marker does not alter the frozen 48-byte vertex layout.
// t3dRdMeshVertex_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdMeshVertex_t ) == 48u, "RD mesh vertex ABI changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_nColorRgba8 ) == 12u, "RD mesh color offset changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_szNormal ) == 16u, "RD mesh normal offset changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_szTangent ) == 24u, "RD mesh tangent offset changed" );
static_assert( offsetof( t3dRdMeshVertex_t, m_szUv ) == 32u, "RD mesh UV offset changed" );

// Production static-instance record: 32 bytes. Position stays float32; rotation
// is a normalized quaternion packed as snorm16x4; scale is binary16x3; color is
// RGBA8. Mesh/material/region state is cell-level and is not redundantly stored
// per instance.
// t3dRdInstanceRecord_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdInstanceRecord_t ) == 32u, "RD instance record ABI changed" );
static_assert( offsetof( t3dRdInstanceRecord_t, m_nQuaternion01 ) == 12u, "RD instance rotation offset changed" );
static_assert( offsetof( t3dRdInstanceRecord_t, m_nColorRgba8 ) == 28u, "RD instance color offset changed" );

// t3dRdInstanceFlags_e centralized in Terrain3DTypes.h.


EA_FORCE_INLINE uint32_t t3dRdInstanceCategoryIndex( const int32_t nCategory ) {
	return nCategory < 0 ? 0u : ( nCategory > 2 ? 2u : ( uint32_t )( nCategory ) );
}

EA_FORCE_INLINE uint32_t t3dRdInstanceTextureFilterMode( const uint32_t nMaterialFilter, const float fOverrides, const uint32_t nCategory ) {
	const uint32_t nChoice = ( ( uint32_t )fOverrides >> ( MIN( nCategory, 2u ) * 4u ) ) & 15u;
	return nChoice > 0u && nChoice <= T3D_VT_FILTER_LINEAR_MIPMAP_ANISOTROPIC + 1u ? nChoice - 1u : nMaterialFilter;
}

EA_FORCE_INLINE uint32_t t3dRdInstanceCategoryFlags( const int32_t nCategory ) {
	return ( t3dRdInstanceCategoryIndex( nCategory ) << T3D_RD_INSTANCE_CATEGORY_SHIFT ) & T3D_RD_INSTANCE_CATEGORY_MASK;
}

// CPU publication descriptor for one logical resident cell/LOD. The renderer
// uses the AABB for coarse rejection, then writes visible-instance indices into
// a single persistent indirection buffer grouped by mesh surface/material.
EA_FORCE_INLINE uint64_t t3dRdRegularMeshSourceKey( const uint16_t nAssetId, const int16_t nLod ) {
	return 0x1000000000000000ull | ( uint64_t )( nAssetId ) | ( ( uint64_t )( ( uint16_t )( nLod ) ) << 16u );
}

EA_FORCE_INLINE uint64_t t3dRdHlodMeshSourceKey( const uint32_t nEntryIndex ) {
	return 0x2000000000000000ull | ( uint64_t )( nEntryIndex + 1u );
}

EA_FORCE_INLINE uint64_t t3dRdOcclusionProxySourceKey( const uint16_t nAssetId ) {
	return 0x3000000000000000ull | ( uint64_t )( nAssetId );
}

EA_FORCE_INLINE bool t3dRdIsOcclusionProxySourceKey( const uint64_t nSourceKey ) {
	return ( nSourceKey & 0xF000000000000000ull ) == 0x3000000000000000ull;
}

// t3dRdInstanceCell_s centralized in Terrain3DTypes.h.



// Batch E HLOD culling descriptor. One descriptor is emitted per HLOD draw
// packet because each surface owns a distinct indirect command while sharing
// the package transform/state record. The compute cull pass only mutates the
// command instance count; geometry/material ownership stays in the shared mesh
// cache and instance renderer.
// t3dRdHlodCullDescriptor_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdHlodCullDescriptor_t ) == 48u, "RD HLOD descriptor ABI changed" );
static_assert( offsetof( t3dRdHlodCullDescriptor_t, m_nVisibilityLayers ) == 32u, "RD HLOD descriptor flags ABI changed" );

// t3dRdInstanceCullDescriptor_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdInstanceCullDescriptor_t ) == 80u, "RD instance cull descriptor ABI changed" );
static_assert( offsetof( t3dRdInstanceCullDescriptor_t, m_nDrawSlot ) == 32u, "RD instance cull draw-slot ABI changed" );
static_assert( offsetof( t3dRdInstanceCullDescriptor_t, m_nCommandIndex ) == 36u, "RD instance cull command ABI changed" );

// P33 retains only the compact CPU-side scalar/raster policy record. Texture
// identity and residency are owned exclusively by the common VT material record.
// t3dRdInstanceMaterialFlags_e centralized in Terrain3DTypes.h.


// t3dRdInstanceMaterial_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdInstanceMaterial_t ) == 64u, "RD instance material ABI changed" );
static_assert( offsetof( t3dRdInstanceMaterial_t, m_fNormalScale ) == 48u, "RD instance normal-scale ABI changed" );


// t3dRdOceanState_s centralized in Terrain3DTypes.h.


// CPU-only control-plane generations captured by the same owner transaction as
// the frame snapshot. These values are not part of a GPU ABI.
// t3dRdTerrainGenerationTuple_s centralized in Terrain3DTypes.h.


typedef struct t3dRdFrameSnapshot_s {

	uint64_t m_nPublicationGeneration = 0u;
	uint64_t m_nGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	uint64_t m_nRegionCacheGeneration = 0u;
	uint64_t m_nTerrainTextureGeneration = 0u;
	uint64_t m_nMaterialTextureGeneration = 0u;
	uint32_t m_nPatchCount = 0u;
	uint32_t m_nMeshSize = 48u;
	uint32_t m_nMaterialLayerCount = 1u;
	// CPU-only WG-A publication classification; GPU material ABI is unchanged.
	uint32_t m_nTerrainMaterialBackend = 1u;
	uint32_t m_nCameraCullMask = UINT32_MAX;
	uint32_t m_nRenderLayers = UINT32_MAX;
	// OWNER-PUBLISHED RENDER POLICY. Render execution must not recover these
	// values from the live Terrain3D node.
	uint32_t m_nEditorExecutionMode = 0u;
	bool m_bEditorAuthoringMode = false;
	bool m_bDominantSunOnly = false;
	float m_szInstanceTextureMipBias[4] = {}; // xyz: category mip bias; w: three 4-bit filter overrides (0 inherits)
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
	t3dRdPatch_t m_szPatches[T3D_MAX_CLIPMAP_LEVELS]{};
	t3dRdMaterialLayer_t m_szMaterials[T3D_RD_MATERIAL_LAYER_COUNT]{};
	t3dRdMaterialConfig_t m_materialConfig{};
	t3dRdEditorOverlayConfig_t m_editorOverlayConfig{};
	t3dRdTerrainGenerationTuple_t m_generations{};
	t3dRdOceanState_t m_ocean{};
} t3dRdFrameSnapshot_t;


// t3dRdFrameUniforms_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdFrameUniforms_t ) == 528u, "RD frame UBO must match GLSL std140 layout" );

EA_FORCE_INLINE void t3dRdFrameUniformsInit( t3dRdFrameUniforms_t *pFrame ) {
	memset( pFrame, 0, sizeof( *pFrame ) );
	pFrame->m_fLightDirectionY = -1.0f;
	pFrame->m_fAmbient = 0.28f;
	pFrame->m_fLightColorR = 1.0f;
	pFrame->m_fLightColorG = 1.0f;
	pFrame->m_fLightColorB = 1.0f;
	pFrame->m_fLightColorA = 1.0f;
	pFrame->m_fHeightMin = s_fRegionHeightMin;
	pFrame->m_fHeightRange = s_fRegionHeightRange;
	pFrame->m_fVertexSpacing = 1.0f;
	pFrame->m_fMeshSize = 48.0f;
	pFrame->m_fMaterialLayerCount = 1.0f;
	pFrame->m_fSubdiv = 1.0f;
	pFrame->m_fFogEnd = 1000000.0f;
	pFrame->m_fFogCurve = 1.0f;
	pFrame->m_fDisplacementScale = 1.0f;
}

// t3dRdDrawIndirectCommand_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdDrawIndirectCommand_t ) == 16u, "RD draw indirect command ABI changed" );

// t3dRdIndexedIndirectCommand_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdIndexedIndirectCommand_t ) == 20u, "RD indexed indirect command ABI changed" );

// P29 freezes one backend-neutral metadata slot for each production mesh
// surface/LOD draw unit. Draw slots are surface slots within a mesh-source
// generation; material identity and immutable surface geometry state therefore
// no longer need to be carried in per-draw push constants.

// t3dInstanceDrawMeta_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dInstanceDrawMeta_t ) == 32u, "RD instance draw metadata ABI changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nMaterial ) == 0u, "RD instance draw material offset changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nOverlayMaterial ) == 4u, "RD instance draw overlay offset changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nFirstIndex ) == 8u, "RD instance draw first-index offset changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nIndexCount ) == 12u, "RD instance draw index-count offset changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nVertexOffset ) == 16u, "RD instance draw vertex-offset changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nFlags ) == 20u, "RD instance draw flags offset changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nVisibleBase ) == 24u, "RD instance draw visible-base offset changed" );
static_assert( offsetof( t3dInstanceDrawMeta_t, m_nReserved ) == 28u, "RD instance draw reserved offset changed" );

// WG-E owns the eventual contents of this table. P29 reserves a separate
// draw-slot-keyed ABI so VT prediction can evolve without revising the frozen
// draw record above.
// t3dInstanceVTPredictMeta_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dInstanceVTPredictMeta_t ) == 16u, "RD instance VT prediction metadata ABI changed" );

// t3dRdCullPushConstants_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdCullPushConstants_t ) == 24u, "RD cull push constants ABI changed" );

EA_FORCE_INLINE void t3dRdCullPushConstantsInit( t3dRdCullPushConstants_t *pConstants ) {
	memset( pConstants, 0, sizeof( *pConstants ) );
	pConstants->m_nMaxPatches = T3D_MAX_CLIPMAP_LEVELS;
}

// t3dRdDrawPushConstants_s centralized in Terrain3DTypes.h.

static_assert( sizeof( t3dRdDrawPushConstants_t ) == 16u, "RD draw push constants ABI changed" );

EA_FORCE_INLINE void t3dRdDrawPushConstantsInit( t3dRdDrawPushConstants_t *pConstants ) {
	memset( pConstants, 0, sizeof( *pConstants ) );
}

#endif // TERRAIN3D_RD_TYPES_H
