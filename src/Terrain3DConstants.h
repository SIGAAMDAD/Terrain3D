// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_CONSTANTS_H
#define TERRAIN3D_CONSTANTS_H

#include "core/terrain_3d_numeric_constants.h"
#include "data/terrain_3d_runtime_page_constants.h"
#include "eastl_config.h"
#include "instances/terrain_3d_instance_cache_constants.h"
#include "memory/terrain_3d_memory_constants.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"

#include <EASTL/functional.h>
#include <EASTL/utility.h>
#include <cstddef>
#include <cstdint>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <limits>
#include <numeric>

// GDExtension uses the godot namespace, custom modules do not.
#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// File Paths (e.g. extras)
#define OCEAN_MATERIAL_PATH "res://addons/terrain_3d/extras/shaders/M_ocean.tres"

// Engine Shortcuts
#define PS PhysicsServer3D::get_singleton()
#define IS_EDITOR Engine::get_singleton()->is_editor_hint()

// Constants

// Centralized native compile-time constants. Keep tunable limits and format constants here.
static constexpr uint32_t s_nProfileVersion = 5u;
static constexpr const char *s_pszProfileKey = "xbox360_experimental";
static constexpr const char *s_pszProfileDisplayName = "Xbox 360 Experimental";
static constexpr uint64_t s_nProfileFnvOffset = 1469598103934665603ull;
static constexpr uint64_t s_nProfileFnvPrime = 1099511628211ull;
static constexpr uint32_t s_nHashGoldenRatio32 = 0x9e3779b9u;
static constexpr uint32_t s_nHashMix32 = 0x85ebca6bu;
static constexpr uint32_t s_nHashMix32Secondary = 0xc2b2ae35u;
static constexpr uint64_t s_nHashGoldenRatio64 = 0x9e3779b97f4a7c15ULL;
static constexpr float s_fUint32MaxFloat = 4294967295.0f;
static constexpr size_t s_nTestHalfEncodingCount = 65536u;
static constexpr size_t s_nTestTransformCount = 37u;
static constexpr int s_nTestNormalWidth = 37;
static constexpr int s_nTestNormalHeight = 19;
static constexpr int s_nTestNormalStride = s_nTestNormalWidth + 2;
static constexpr size_t s_nTestErosionCount = 4099u;
static constexpr size_t s_nTestNonFiniteErosionCount = 16u;
static constexpr size_t s_nTestDispatchCount = 1024u;
static constexpr size_t s_nTestDispatchRounds = 32u;
static constexpr size_t s_nTestDispatchIterations = 64u;
static constexpr size_t s_nTestEastlAlignment = 64u;
static constexpr size_t s_nTestEastlOffset = 13u;
static constexpr uint8_t s_nTestControlFlag = 1u << 0u;
static constexpr const char *s_pszCacheThrashSubsystemNames[] = {
	"gpu_terrain_pages",
	"cpu_terrain_pages",
	"instance_pages",
};
static constexpr const char *s_pszMemorySubsystemNames[] = {
	"gpu_terrain_pages",
	"cpu_terrain_pages",
	"instance_pages",
	"baked_lighting",
	"hlod_proxies",
	"shader_cache",
};
static constexpr size_t s_nMemorySubsystemNameCount =
		sizeof( s_pszMemorySubsystemNames ) / sizeof( s_pszMemorySubsystemNames[0] );
static constexpr const char *s_pszEditorToolNames[] = {
	"Region",
	"Sculpt",
	"Height",
	"Texture",
	"Color",
	"Roughness",
	"Auto Shader",
	"Holes",
	"Navigation",
	"Instancer",
	"Angle",
	"Scale",
	"Landform",
	"TOOL_MAX",
};
static constexpr const char *s_pszEditorOperationNames[] = {
	"Add",
	"Subtract",
	"Replace",
	"Average",
	"Gradient",
	"OP_MAX",
};
static constexpr const char *s_pszShaderStaticMacroNames[] = {
	"T3D_TERRAIN_SAMPLING_MODE",
	"T3D_TERRAIN_NORMAL_MODE",
	"T3D_COLOR_SAMPLING_MODE",
	"T3D_PROJECTION_MODE",
	"T3D_TEXTURE_SURFACE_MODE",
	"T3D_SAMPLE_SURFACE_DATA",
	"T3D_OUTPUT_ROUGHNESS",
	"T3D_OUTPUT_SPECULAR",
	"T3D_OUTPUT_NORMAL_MAP",
	"T3D_OUTPUT_AO",
	"T3D_SHADING_MODE",
	"T3D_DETILING_MODE",
	"T3D_WORLD_NOISE_SHADER_MODE",
	"T3D_FAST_BLEND_CURVE",
	"T3D_CONTROL_MAPS_IMPLICIT",
	"T3D_COLOR_MAPS_IMPLICIT",
	"T3D_AUTO_SHADER_ENABLED",
};
static constexpr GeometryInstance3D::ShadowCastingSetting s_shadowCastingOn = GeometryInstance3D::SHADOW_CASTING_SETTING_ON;
static constexpr GeometryInstance3D::ShadowCastingSetting s_shadowCastingOff = GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
static constexpr GeometryInstance3D::ShadowCastingSetting s_shadowCastingOnly =
		GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY;
static constexpr int s_nMaxTextureAssets = 32;
static constexpr int s_nMaxMeshAssets = 256;
static constexpr real_t s_fDataCurrentVersion = 0.94f;
static constexpr float s_fRegionHeightMin = -384.0f;
static constexpr float s_fRegionHeightMax = 768.0f;
static constexpr float s_fRegionHeightRange = s_fRegionHeightMax - s_fRegionHeightMin;
static constexpr float s_fRegionHeightInvRange = 1.0f / s_fRegionHeightRange;
static constexpr float s_fRegionHeightU16Scale = 65535.0f;
static constexpr uint32_t s_nRegionDefaultControl = 1u;
static constexpr uint32_t s_nAutoshaderMaterialMask = ( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u );
static constexpr float s_fDegreesPerRadian = 57.29577951308232f;
static constexpr float s_fSqrtTwo = 1.4142135623730951f;
static constexpr float s_fLargeDistance = 1.0e20f;
static constexpr uint32_t s_nLandformMaterialMask = s_nAutoshaderMaterialMask | 0x1u;
static constexpr float s_fLandformEpsilon = 0.0001f;
static constexpr int32_t s_nErosionDirectionCount = 4;
static constexpr int32_t s_szErosionDirectionX[s_nErosionDirectionCount] = { -1, 1, 0, 0 };
static constexpr int32_t s_szErosionDirectionY[s_nErosionDirectionCount] = { 0, 0, -1, 1 };
static constexpr int32_t s_szErosionOppositeDirection[s_nErosionDirectionCount] = { 1, 0, 3, 2 };
static constexpr float s_fErosionEpsilon = 1.0e-6f;
static constexpr uint64_t s_nErosionFnvOffset = 1469598103934665603ULL;
static constexpr uint64_t s_nErosionFnvPrime = 1099511628211ULL;
static constexpr size_t s_nErosionHalfDecodeBlock = 256u;
static constexpr float s_fClipmapPartStatic = 0.0f;
static constexpr float s_fClipmapPartEdgeA0 = 1.0f;
static constexpr float s_fClipmapPartEdgeA1 = 2.0f;
static constexpr float s_fClipmapPartEdgeB0 = 3.0f;
static constexpr float s_fClipmapPartEdgeB1 = 4.0f;
static constexpr int s_nMultimeshStrideFloats = 16;
static constexpr uint32_t s_nComputeWorkgroupSize = 64u;
static constexpr int s_nShaderMaxConditionalDepth = 32;
static constexpr uint32_t s_nInstancePageMagic = 0x49334454u;
static constexpr uint16_t s_nInstancePageVersion = 2u;
static constexpr uint16_t s_nInstancePageLegacyVersion = 1u;
static constexpr size_t s_nInstancePageHeaderBytes = 16u;
static constexpr size_t s_nInstancePageMeshHeaderBytes = 12u;
static constexpr size_t s_nInstancePageLegacyMeshHeaderBytes = 8u;
static constexpr size_t s_nInstancePageCellHeaderBytes = 12u;
static constexpr uint16_t s_nInstancePageCellColors = 1u << 0u;
static constexpr uint16_t s_nInstancePageCellOwners = 1u << 1u;
static constexpr uint16_t s_nInstancePageCellFlags = s_nInstancePageCellColors | s_nInstancePageCellOwners;
static constexpr uint32_t s_nActiveAccessReaderMask = 0x7fffffffu;
static constexpr uint32_t s_nActiveAccessWriterBit = 0x80000000u;
static constexpr size_t s_nActiveStorageAlignment = 64u;
static constexpr int s_nAutoshaderModeLegacyRuntime = 0;
static constexpr int s_nAutoshaderModeBaked = 1;
static constexpr int s_nAutoshaderModeDisabled = 2;
static constexpr int s_nAutoshaderModeEditorPreview = 3;
static constexpr size_t s_nBakeCachelineBytes = 64u;
static constexpr size_t s_nComputeScratchRetainLimit = 32u * 1024u * 1024u;
static constexpr size_t s_nCpuCacheBytesPerTexel = 12u;
static constexpr int s_nCpuCacheMaxLayers = 64;
static constexpr size_t s_nCpuCacheMib = 1024u * 1024u;
static constexpr int s_nCpuCacheMinSampleLayers = 4;
static constexpr int s_nCpuRegionCacheMaxBudgetMb = 128;
static constexpr int s_nCpuRegionCacheMinBudgetMb = 1;
static constexpr int s_nCpuRegionLoadMaxBudgetMb = 64;
static constexpr int s_nCpuRegionLoadMaxLayers = 64;
static constexpr size_t s_nDefaultResidentPoolBudget = 64u * 1024u * 1024u;
static constexpr int s_nDiagnosticsVersion = 1;
static constexpr uint32_t s_nDirectionalReceiverLayer = 1u << 19u;
static constexpr int32_t s_nErosionRegionLookupCapacity = 4096;
static constexpr int32_t s_nErosionTextureProfileCount = 32;
static constexpr uint32_t s_nFeatureAltValue = 1u << 0u;
static constexpr uint32_t s_nFeatureAngleValue = 1u << 3u;
static constexpr uint32_t s_nFeatureCenterValidValue = 1u << 1u;
static constexpr uint32_t s_nFeatureScaleValue = 1u << 4u;
static constexpr uint32_t s_nFeatureTextureValue = 1u << 2u;
static constexpr int s_nGpuHeightCacheMaxLayers = 512;
static constexpr int s_nGpuHeightCacheMinSpareLayers = 8;
static constexpr uint64_t s_nGpuFarAtlasHeaderBytes = 52u;
static constexpr uint64_t s_nGpuFarAtlasMagic = 0x5444334641523031ull;
static constexpr uint32_t s_nGpuFarAtlasVersion = 7u;
static constexpr int s_nGpuRegionCacheMaxBudgetMb = 128;
static constexpr int s_nGpuRegionCacheMinBudgetMb = 1;
static constexpr int s_nGpuRegionUploadMaxBudgetMb = 64;
static constexpr int s_nGpuRegionUploadMaxLayers = 64;
static constexpr int s_nInstanceDefaultCellSize = 32;
static constexpr int s_nInstanceMaxCellsPerAxis = 256;
static constexpr int s_nInstanceMaxCellSize = 128;
static constexpr int s_nInstanceMaxRegionSize = 2048;
static constexpr int s_nInstanceMinCellSize = 8;
static constexpr int s_nInstanceMinRegionSize = 64;
static constexpr int s_nInstanceRegionCacheMaxBudgetMb = 128;
static constexpr int s_nInstanceRegionCacheMinBudgetMb = 1;
static constexpr int s_nInstanceRegionLoadMaxBudgetMb = 64;
static constexpr int s_nInstanceRegionLoadMaxPages = 64;
static constexpr int s_nInstanceTransitionBucketCellSpan = 4;
static constexpr uint32_t s_nInstanceWhiteRgba8 = 0xffffffffu;
static constexpr uint32_t s_nLightingLayerFoliage = 1u << 15u;
static constexpr uint32_t s_nLightingLayerLarge = 1u << 17u;
static constexpr uint32_t s_nLightingLayerMedium = 1u << 16u;
static constexpr uint32_t s_nLightingLayerRegional = 1u << 18u;
static constexpr int s_nLightmapActiveInstanceLimit = 8;
static constexpr uint64_t s_nLightmapFnvOffset = 1469598103934665603ull;
static constexpr uint64_t s_nLightmapFnvPrime = 1099511628211ull;
static constexpr int s_nMaxClipmapLevels = 16;
static constexpr int s_nMaxRegionBlendSamples = 2048;
// Full-region heightmap collision samples region_size + 1 vertices. Keep the
// axis scratch large enough for the maximum supported 2048-texel region.
static constexpr int s_nMaxCollisionAxisSamples = s_nMaxRegionBlendSamples + 1;
static constexpr int s_nMaxRenderBatches = 5;
static constexpr int s_nMaxShaderVariantCacheLimit = 256;
static constexpr int s_nMaxShaderVariantPrewarmLimit = 64;
static constexpr int s_nMaxShaderVariantSourceBudgetMb = 256;
static constexpr int s_nMaxTileInstances = 16;
static constexpr uint64_t s_nMemoryMib = 1024ULL * 1024ULL;
static constexpr int s_nMemoryPolicyIntervalFrames = 30;
static constexpr int s_nMeshMaxLodCount = 10;
static constexpr int s_nMeshShadowLodId = -1;
static constexpr int s_nMinRegionSize = 64;
static constexpr int s_nMinShaderVariantCacheLimit = 8;
static constexpr int s_nMinShaderVariantPrewarmLimit = 1;
static constexpr int s_nMinShaderVariantSourceBudgetMb = 1;
static constexpr uint32_t s_nOpAddValue = 0u;
static constexpr uint32_t s_nOpGradientValue = 4u;
static constexpr uint32_t s_nOpReplaceValue = 2u;
static constexpr uint32_t s_nOpSubtractValue = 1u;
static constexpr int s_nPairInstances = 2;
static constexpr int s_nRenderControlBlendRadius = 2;
static constexpr int s_szRenderControlKernel[5] = { 1, 2, 3, 2, 1 };
static constexpr uint32_t s_nRenderControlMaterialMask = ( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u );
static constexpr size_t s_nResidentCacheAlignment = 64u;
static constexpr size_t s_nResidentInvalidOffset = ( size_t )( -1 );
static constexpr int s_nRingTileInstances = 12;
static constexpr size_t s_nRuntimeProfileHistoryCapacity = 600u;
static constexpr int s_nSamplerCacheSize = 8;
static constexpr uint32_t s_nToolAutoshaderValue = 6u;
static constexpr uint32_t s_nToolColorValue = 4u;
static constexpr uint32_t s_nToolHeightValue = 2u;
static constexpr uint32_t s_nToolHolesValue = 7u;
static constexpr uint32_t s_nToolNavigationValue = 8u;
static constexpr uint32_t s_nToolRoughnessValue = 5u;
static constexpr uint32_t s_nToolTextureValue = 3u;
static constexpr uint32_t s_nTypeColorValue = 2u;
static constexpr uint32_t s_nTypeControlValue = 1u;
static constexpr uint32_t s_nTypeHeightValue = 0u;
static constexpr uint32_t s_nTypeMaxValue = 3u;
static constexpr int s_nWorldTexels = 4096;
static constexpr size_t s_nXbox360CpuMapCeilingBytes = 64u * 1024u * 1024u;
static constexpr int s_nActiveMinRegionSize = s_nMinRegionSize;
static constexpr int s_nActiveWorldTexels = s_nWorldTexels;
static constexpr size_t s_nBakeUvCachelineElements =
		s_nBakeCachelineBytes / std::gcd( s_nBakeCachelineBytes, sizeof( Vector2 ) );
static constexpr size_t s_nBakeVertexCachelineElements =
		s_nBakeCachelineBytes / std::gcd( s_nBakeCachelineBytes, sizeof( Vector3 ) );
static constexpr size_t s_nGpuCacheFarColorBytes =
		( size_t )( s_nGpuCacheFarHeightSide ) * ( size_t )( s_nGpuCacheFarHeightSide ) * 4u;
static constexpr size_t s_nGpuCacheFarHeightBytes =
		( size_t )( s_nGpuCacheFarHeightSide ) * ( size_t )( s_nGpuCacheFarHeightSide ) * 2u;
static constexpr int s_nGpuFarHeightSize = s_nGpuCacheFarHeightSide;
static constexpr uint32_t s_nLightingClassLayerMask =
		s_nLightingLayerFoliage | s_nLightingLayerMedium | s_nLightingLayerLarge | s_nLightingLayerRegional;
static constexpr int s_nMaxBatchInstances = s_nMaxClipmapLevels;
static constexpr int s_nMaxBufferShaderVariantCacheLimit = s_nMaxShaderVariantCacheLimit / 2;
static constexpr int s_nMaxEditorShaderVariantCacheLimit = s_nMaxShaderVariantCacheLimit / 4;
static constexpr int s_nMaxRegionMapSize = s_nWorldTexels / s_nMinRegionSize;
static constexpr uint64_t s_nMemoryPolicyHysteresisBytes = 2ULL * s_nMemoryMib;
static constexpr int s_nActiveMaxRegionMapSize = s_nMaxRegionMapSize;
static const Vector2i s_regionMapVSize{ s_nActiveMaxRegionMapSize, s_nActiveMaxRegionMapSize };
static constexpr size_t s_nBakeTaskAlignmentElements =
		std::lcm( s_nBakeVertexCachelineElements, s_nBakeUvCachelineElements );
static constexpr uint32_t s_nLightingManagedLayerMask = s_nLightingClassLayerMask | s_nDirectionalReceiverLayer;
static constexpr int s_nMaxRegionSlots = s_nMaxRegionMapSize * s_nMaxRegionMapSize;
static constexpr int s_nResidentWorldSize = s_nMaxRegionMapSize;
static constexpr int s_nActiveMaxRegionSlots = s_nMaxRegionSlots;
static constexpr size_t s_nInstanceRegionTableCapacity = ( size_t )( s_nMaxRegionSlots );
static constexpr int s_nResidentWorldHalf = s_nResidentWorldSize / 2;
static constexpr int s_nResidentWorldSlotCount = s_nMaxRegionSlots;
static constexpr size_t s_nInstanceRegionUpdateCapacity = s_nInstanceRegionTableCapacity;
typedef enum t3dPerformanceProfileOwner_e : uint8_t {
	T3D_PROFILE_OWNER_TERRAIN,
	T3D_PROFILE_OWNER_MATERIAL,
} t3dPerformanceProfileOwner_t;

typedef enum t3dPerformanceProfileValueKind_e : uint8_t {
	T3D_PROFILE_VALUE_INT,
	T3D_PROFILE_VALUE_FLOAT,
	T3D_PROFILE_VALUE_BOOL,
} t3dPerformanceProfileValueKind_t;

typedef struct t3dPerformanceProfileSetting_s {
	t3dPerformanceProfileOwner_t m_owner;
	t3dPerformanceProfileValueKind_t m_kind;
	const char *m_pszProperty;
	int64_t m_nIntValue;
	double m_fFloatValue;
	bool m_bBoolValue;
} t3dPerformanceProfileSetting_t;

#define T3D_PROFILE_TERRAIN_INT( name, value ) \
	{ T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_INT, name, ( int64_t )( value ), 0.0, false },
#define T3D_PROFILE_TERRAIN_FLOAT( name, value ) \
	{ T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_FLOAT, name, 0, ( double )( value ), false },
#define T3D_PROFILE_TERRAIN_BOOL( name, value ) \
	{ T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_BOOL, name, 0, 0.0, ( bool )( value ) },
#define T3D_PROFILE_MATERIAL_INT( name, value ) \
	{ T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_INT, name, ( int64_t )( value ), 0.0, false },
#define T3D_PROFILE_MATERIAL_FLOAT( name, value ) \
	{ T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_FLOAT, name, 0, ( double )( value ), false },
#define T3D_PROFILE_MATERIAL_BOOL( name, value ) \
	{ T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_BOOL, name, 0, 0.0, ( bool )( value ) },

inline constexpr t3dPerformanceProfileSetting_t g_szXbox360ExperimentalSettings[] = {
#include "streaming/terrain_3d_performance_profile_manifest.inc"
};
inline constexpr size_t g_nXbox360ExperimentalSettingCount =
		sizeof( g_szXbox360ExperimentalSettings ) / sizeof( g_szXbox360ExperimentalSettings[0] );

#undef T3D_PROFILE_TERRAIN_INT
#undef T3D_PROFILE_TERRAIN_FLOAT
#undef T3D_PROFILE_TERRAIN_BOOL
#undef T3D_PROFILE_MATERIAL_INT
#undef T3D_PROFILE_MATERIAL_FLOAT
#undef T3D_PROFILE_MATERIAL_BOOL

// End centralized native compile-time constants.

static const Color s_colorNan{ NAN, NAN, NAN, NAN };
static const Color s_colorWhite{ 1.0f, 1.0f, 1.0f, 1.0f };
static const Image::Format s_szRegionMapFormats[s_nTypeMaxValue + 1u] = {
	Image::FORMAT_R16,
	Image::FORMAT_RF,
	Image::FORMAT_RGBA8,
	Image::Format( s_nTypeMaxValue ),
};
static constexpr const char *s_pszRegionMapTypeNames[s_nTypeMaxValue + 1u] = {
	"TYPE_HEIGHT",
	"TYPE_CONTROL",
	"TYPE_COLOR",
	"TYPE_MAX",
};
#define COLOR_BLACK Color( 0.0f, 0.0f, 0.0f, 1.0f )
#define COLOR_ROUGHNESS Color( 1.0f, 1.0f, 1.0f, 0.5f )
#define COLOR_CHECKED Color( 1.f, 1.f, 1.0f, -1.0f )
#define COLOR_NORMAL Color( 0.5f, 0.5f, 1.0f, 1.0f )
static constexpr float s_fControlAutoFloat = std::numeric_limits<float>::denorm_min();
#define COLOR_CONTROL Color( s_fControlAutoFloat, 0.f, 0.f, 1.0f )
static const Color s_szRegionMapDefaultColors[s_nTypeMaxValue + 1u] = {
	COLOR_BLACK,
	COLOR_CONTROL,
	COLOR_ROUGHNESS,
	s_colorNan,
};

// For consistency between MSVC, gcc, clang
#ifndef FLT_MAX
#define FLT_MAX __FLT_MAX__
#endif

// Warning flags are stored in an 8-bit mask.
#define WARN_MISMATCHED_SIZE 0x01
#define WARN_MISMATCHED_FORMAT 0x02
#define WARN_MISMATCHED_MIPMAPS 0x04
#define WARN_ALL 0xFF

// Global Types

#define V2( x ) Vector2( x, x )
#define V2I( x ) Vector2i( x, x )
#define V3( x ) Vector3( x, x, x )
static const Vector2 s_v2Zero{ 0.f, 0.f };
static const Vector2 s_v2Max{ FLT_MAX, FLT_MAX };
static const Vector2i s_v2iZero{ 0, 0 };
static const Vector2i s_v2iMax{ INT32_MAX, INT32_MAX };
static const Vector3 s_v3Zero{ 0.f, 0.f, 0.f };
static const Vector3 s_v3Max{ FLT_MAX, FLT_MAX, FLT_MAX };
static const Vector3 s_v3Nan{ NAN, NAN, NAN };
static const Vector3 s_v3Up{ 0.f, 1.f, 0.f };
// static const Vector3 V3_DOWN{ 0.f, -1.f, 0.f };

typedef struct t3dVector2iHash_s {
	std::size_t operator()( const Vector2i &v ) const {
		std::size_t h1 = eastl::hash<int>()( v.x );
		std::size_t h2 = eastl::hash<int>()( v.y );
		return h1 ^ ( h2 << 1 );
	}
} t3dVector2iHash_t;

typedef struct t3dPairVector2iIntHash_s {
	std::size_t operator()( const eastl::pair<Vector2i, int> &p ) const {
		std::size_t h1 = t3dVector2iHash_t{}( p.first );
		std::size_t h2 = eastl::hash<int>{}( p.second );
		return h1 ^ ( h2 << 1 );
	}
} t3dPairVector2iIntHash_t;

typedef struct t3dVector3Hash_s {
	std::size_t operator()( const Vector3 &v ) const {
		std::size_t h1 = eastl::hash<float>()( v.x );
		std::size_t h2 = eastl::hash<float>()( v.y );
		std::size_t h3 = eastl::hash<float>()( v.z );
		return h1 ^ ( h2 << 1 ) ^ ( h3 << 2 );
	}
} t3dVector3Hash_t;

// Set class name for logger.h

#define CLASS_NAME() \
	const String __class__ = get_class_static() + String( "#" ) + String::num_uint64( get_instance_id() ).right( 4 );

#define CLASS_NAME_STATIC( pszName ) static inline const char *__class__ = pszName;

// Validation macros

#define ASSERT( cond, ret ) \
	if ( !( cond ) ) { \
		UtilityFunctions::push_error( "Assertion '", #cond, "' failed at ", __FILE__, ":", __LINE__ ); \
		return ret; \
	}

#define VOID // a return value for void, to avoid compiler warnings

#define IS_INIT( ret ) \
	if ( !m_pTerrain ) { \
		return ret; \
	}

#define IS_INIT_MESG( mesg, ret ) \
	if ( !m_pTerrain ) { \
		LOG( ERROR, mesg ); \
		return ret; \
	}

#define IS_INIT_COND( cond, ret ) \
	if ( !m_pTerrain || cond ) { \
		return ret; \
	}

#define IS_INIT_COND_MESG( cond, mesg, ret ) \
	if ( !m_pTerrain || cond ) { \
		LOG( ERROR, mesg ); \
		return ret; \
	}

#define IS_INSTANCER_INIT( ret ) \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) { \
		return ret; \
	}

#define IS_INSTANCER_INIT_MESG( mesg, ret ) \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) { \
		LOG( ERROR, mesg ); \
		return ret; \
	}

#define IS_DATA_INIT( ret ) \
	if ( !m_pTerrain || !m_pTerrain->getData() ) { \
		return ret; \
	}

#define IS_DATA_INIT_MESG( mesg, ret ) \
	if ( !m_pTerrain || !m_pTerrain->getData() ) { \
		LOG( ERROR, mesg ); \
		return ret; \
	}

#endif // TERRAIN3D_CONSTANTS_H
