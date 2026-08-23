// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_CLASS_H
#define TERRAIN3D_CLASS_H

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_assets.h"
#include "core/target_node_3d.h"
#include "core/terrain_3d_diagnostics.h"
#include "data/terrain_3d_data.h"
#include "eastl_config.h"
#include "instances/terrain_3d_instancer.h"
#include "memory/terrain_3d_memory_arena.h"
#include "memory/terrain_3d_memory_budget.h"
#include "memory/terrain_3d_memory_zone.h"
#include "physics/terrain_3d_collision.h"
#include "rendering/terrain_3d_compute.h"
#include "rendering/terrain_3d_material.h"
#include "rendering/terrain_3d_mesher.h"
#include "streaming/terrain_3d_adaptive_budget_policy.h"
#include "streaming/terrain_3d_boundary_phase_policy.h"
#include "streaming/terrain_3d_streaming_policy.h"

#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <atomic>
#include <godot_cpp/classes/array_occluder3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <mutex>

class Terrain3DEditor;
class Terrain3DNodeStreamer;
class Terrain3DLightmapStreamer;
class Terrain3DHLODStreamer;
class Terrain3DRenderCoordinator;


EA_FORCE_INLINE String t3dDefaultRegionLightmapOutputDirectory( const String &rDataDirectory ) {
	const String directory = rDataDirectory.strip_edges().trim_suffix( "/" );
	return directory.is_empty() ? String() : directory.path_join( "lighting" );
}

typedef enum t3dDebugLevel_e {
	MESG = -2, // Always print except in release builds
	WARN = -1, // Always print except in release builds
	ERROR = 0, // Always print except in release builds
	INFO = 1, // Print every function call and important entries
	DEBUG = 2, // Print details within functions
	EXTREME = 3, // Continuous operations like snapping
} t3dDebugLevel_t;

typedef enum t3dPerformanceTarget_e {
	PERFORMANCE_TARGET_CUSTOM,
	PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL,
} t3dPerformanceTarget_t;

typedef enum t3dPerformanceProfileStatus_e {
	PERFORMANCE_PROFILE_CUSTOM,
	PERFORMANCE_PROFILE_EXACT,
	PERFORMANCE_PROFILE_MODIFIED,
} t3dPerformanceProfileStatus_t;

typedef enum t3dEnvironmentShadowQuality_e {
	ENVIRONMENT_SHADOW_LOW,
	ENVIRONMENT_SHADOW_NORMAL,
	ENVIRONMENT_SHADOW_HIGH,
	ENVIRONMENT_SHADOW_ULTRA,
} t3dEnvironmentShadowQuality_t;

typedef enum t3dRegionSize_e {
	SIZE_64 = 64,
	SIZE_128 = 128,
	SIZE_256 = 256,
	SIZE_512 = 512,
	SIZE_1024 = 1024,
	SIZE_2048 = 2048,
} t3dRegionSize_t;

typedef enum t3dOceanPerformancePreset_e {
	OCEAN_PRESET_CUSTOM,
	OCEAN_PRESET_BORDER_MINIMUM,
	OCEAN_PRESET_BORDER_BALANCED,
	OCEAN_PRESET_HIGH,
	OCEAN_PRESET_CINEMATIC,
} t3dOceanPerformancePreset_t;

typedef enum t3dOceanRefractionMode_e {
	OCEAN_REFRACTION_OFF,
	OCEAN_REFRACTION_DEPTH_TINT,
	OCEAN_REFRACTION_FULL
} t3dOceanRefractionMode_t;

typedef enum t3dOceanFoamMode_e {
	OCEAN_FOAM_OFF,
	OCEAN_FOAM_SHORE,
	OCEAN_FOAM_FULL
} t3dOceanFoamMode_t;

typedef enum t3dOceanScatteringMode_e {
	OCEAN_SCATTERING_OFF,
	OCEAN_SCATTERING_APPROXIMATE,
	OCEAN_SCATTERING_FULL
} t3dOceanScatteringMode_t;

typedef enum t3dOceanShadingMode_e {
	OCEAN_SHADING_LAMBERT_DIFFUSE,
	OCEAN_SHADING_LAMBERT_GGX,
	OCEAN_SHADING_BURLEY_GGX,
	OCEAN_SHADING_UNSHADED,
	OCEAN_SHADING_LAMBERT_VERTEX
} t3dOceanShadingMode_t;

typedef struct t3dTriangleBuffer_s {
	eastl::vector<Vector3> m_ownedVertices;
	eastl::vector<Vector2> m_ownedUvs;
	Vector3 *m_pExternalVertices = nullptr;
	Vector2 *m_pExternalUvs = nullptr;
	size_t m_nExternalCapacity = 0u;
	size_t m_nExternalSize = 0u;
	bool m_bOverflowed = false;

	EA_FORCE_INLINE void bindExternal( Vector3 *const pVertices, Vector2 *const pUvs, const size_t nCapacity ) {
		m_pExternalVertices = pVertices;
		m_pExternalUvs = pUvs;
		m_nExternalCapacity = nCapacity;
		m_nExternalSize = 0u;
		m_bOverflowed = false;
	}

	void reserve( const size_t nCapacity, const bool bUvs ) {
		m_ownedVertices.reserve( nCapacity );
		if ( bUvs ) {
			m_ownedUvs.reserve( nCapacity );
		}
	}

	void appendTriangle( const Vector3 &rA, const Vector3 &rB, const Vector3 &rC, const bool bUvs ) {
		if ( m_pExternalVertices ) {
			if ( m_nExternalSize + 3u > m_nExternalCapacity ) {
				m_bOverflowed = true;
				return;
			}
			m_pExternalVertices[m_nExternalSize] = rA;
			m_pExternalVertices[m_nExternalSize + 1u] = rB;
			m_pExternalVertices[m_nExternalSize + 2u] = rC;
			if ( bUvs && m_pExternalUvs ) {
				m_pExternalUvs[m_nExternalSize] = Vector2( rA.x, rA.z );
				m_pExternalUvs[m_nExternalSize + 1u] = Vector2( rB.x, rB.z );
				m_pExternalUvs[m_nExternalSize + 2u] = Vector2( rC.x, rC.z );
			}
			m_nExternalSize += 3u;
			return;
		}

		m_ownedVertices.push_back( rA );
		m_ownedVertices.push_back( rB );
		m_ownedVertices.push_back( rC );
		if ( bUvs ) {
			m_ownedUvs.push_back( Vector2( rA.x, rA.z ) );
			m_ownedUvs.push_back( Vector2( rB.x, rB.z ) );
			m_ownedUvs.push_back( Vector2( rC.x, rC.z ) );
		}
	}

	EA_FORCE_INLINE size_t size( void ) const {
		return m_pExternalVertices ? m_nExternalSize : m_ownedVertices.size();
	}

	EA_FORCE_INLINE size_t uvSize( void ) const {
		return m_pExternalVertices ? ( m_pExternalUvs ? m_nExternalSize : 0u ) : m_ownedUvs.size();
	}

	EA_FORCE_INLINE const Vector3 *vertexData( void ) const {
		return m_pExternalVertices ? m_pExternalVertices : m_ownedVertices.data();
	}

	EA_FORCE_INLINE const Vector2 *uvData( void ) const {
		return m_pExternalVertices ? m_pExternalUvs : m_ownedUvs.data();
	}
} t3dTriangleBuffer_t;

typedef struct t3dBakeRowTask_s {
	int32_t m_nXStart = 0;
	int32_t m_nXEnd = 0;
	int32_t m_nZStart = 0;
	int32_t m_nZEnd = 0;
	size_t m_iOutputOffset = 0u;
	size_t m_nOutputCapacity = 0u;
} t3dBakeRowTask_t;

typedef struct t3dBakeRegionSnapshot_s {
	Vector2i m_location;
	PackedByteArray m_heightData;
	PackedByteArray m_controlData;
	const uint8_t *m_pHeightBytes = nullptr;
	const uint8_t *m_pControlBytes = nullptr;
} t3dBakeRegionSnapshot_t;

typedef struct t3dBakeSnapshot_s {
	int32_t m_nRegionSize = 0;
	real_t m_fVertexSpacing = 1.f;
	int32_t m_szRegionLookup[s_nActiveMaxRegionMapSize * s_nActiveMaxRegionMapSize] = {};
	eastl::vector<t3dBakeRegionSnapshot_t> m_regions;

	const t3dBakeRegionSnapshot_t *getRegion( const int32_t iX, const int32_t iZ ) const;
	void getSample( const int32_t iX, const int32_t iZ, uint32_t &nControl, real_t &fHeight ) const;
	bool getMinimumRect( int32_t iX, int32_t iZ, int32_t nWidth, int32_t nHeight, real_t &fHeight ) const;
	Vector3 getMeshVertex( const int32_t nLod,
			const t3dHeightFilter_t filter,
			const int32_t iX,
			const int32_t iZ,
			uint32_t &nControl ) const;
} t3dBakeSnapshot_t;

typedef struct t3dBakeTaskContext_s {
	const Terrain3D *m_pTerrain = nullptr;
	const t3dBakeSnapshot_t *m_pSnapshot = nullptr;
	const eastl::vector<t3dBakeRowTask_t> *m_pTasks = nullptr;
	eastl::vector<t3dTriangleBuffer_t> *m_pBuffers = nullptr;
	int32_t m_nStep = 1;
	int32_t m_nLod = 0;
	t3dHeightFilter_t m_filter = HEIGHT_FILTER_NEAREST;
	bool m_bGenerateUvs = false;
	bool m_bRequireNav = false;
} t3dBakeTaskContext_t;

class Terrain3D : public Node3D {
	GDCLASS( Terrain3D, Node3D );
	CLASS_NAME();

public: // Constants
private:
	String m_version = "1.1.0-dev";
	String m_dataDirectory;
	bool m_bIsInsideWorld = false;
	bool m_bInitialized = false;
	uint8_t m_nWarnings = 0u;
	// Runtime memory ownership. Declaration order is intentional: allocator-backed
	// containers are destroyed before the zone and its arena backing store.
	Terrain3DMemoryArena m_runtimeMemoryArena;
	Terrain3DMemoryZone m_runtimeMemoryZone;
	t3dMemoryArenaSlice_t m_runtimeActiveHeightSlice;
	t3dMemoryArenaSlice_t m_runtimeZoneSlice;
	bool m_bRuntimeMemoryReady = false;

	// Object references
	Terrain3DData *m_pData = nullptr;
	Ref<Terrain3DAssets> m_assets;
	Ref<Resource> m_autoShaderProfile;
	Terrain3DCollision *m_pCollision = nullptr;
	Terrain3DInstancer *m_pInstancer = nullptr;
	Terrain3DNodeStreamer *m_pNodeStreamer = nullptr;
	Terrain3DLightmapStreamer *m_pLightmapStreamer = nullptr;
	Terrain3DHLODStreamer *m_pHlodStreamer = nullptr;
	Terrain3DEditor *m_pEditor = nullptr;
	Object *m_pEditorPlugin = nullptr;

	// Regions
	t3dRegionSize_t m_nRegionSize = SIZE_128;
	bool m_bSave16Bit = false;
	t3dPerformanceTarget_t m_performanceTarget = PERFORMANCE_TARGET_CUSTOM;
	t3dPerformanceProfileStatus_t m_performanceProfileStatus = PERFORMANCE_PROFILE_CUSTOM;
	bool m_bApplyingPerformanceTarget = false;
	bool m_bPerformanceProfileMaterialPendingApply = false;
	// Runtime settings remain stored in every mode. Authoring changes only the
	// effective editor residency policy; profiling uses the exact runtime path.
	t3dEditorExecutionMode_t m_editorExecutionMode = EDITOR_EXECUTION_AUTHORING;
	// Editor-only node preview keeps terrain authoring residency intact while
	// exercising the shipping dynamic-node streamer around the editor camera.
	bool m_bEditorNodeStreamingPreview = false;
	// HLOD preview uses shipping representation selection while terrain stays in Authoring mode.
	bool m_bEditorHlodPreview = false;

	// Unified logical region-streaming policy. Every subsystem consumes this map;
	// no renderer/object cache is allowed to invent a separate world footprint.
	t3dRegionStreamingConfig_t m_regionStreamingConfig{};
	uint8_t m_szRegionStreamingStates[s_nMaxRegionSlots] = {};
	uint8_t m_szRegionStreamingPrefetch[s_nMaxRegionSlots] = {};
	Vector2i m_regionStreamingCenter = s_v2iMax;
	Vector2i m_regionStreamingPredictedCenter = s_v2iMax;
	Vector3 m_regionStreamingPreviousWorldPosition = s_v3Max;
	double m_fRegionStreamingVelocityX = 0.0;
	double m_fRegionStreamingVelocityZ = 0.0;
	int m_nRegionStreamingActiveCount = 0;
	int m_nRegionStreamingHotCount = 0;
	int m_nRegionStreamingColdCount = 0;
	int m_nRegionStreamingUnloadedCount = 0;
	int m_nRegionStreamingPrefetchCount = 0;
	uint64_t m_nRegionStreamingGeneration = 0u;
	bool m_bRegionStreamingDirty = true;
	t3dRuntimeProfileAccumulator_t m_runtimeProfileAccumulator;
	t3dRuntimeProfileHistory_t *m_pRuntimeProfileHistory = nullptr;
	t3dRuntimeProfileSample_t m_runtimeProfileLastSample;
	uint64_t m_nRuntimeProfileSampleCount = 0u;
	uint64_t m_nRuntimeProfileShaderUpdatesSeen = 0u;
	uint64_t m_nRuntimeProfileShaderCreationsSeen = 0u;
	bool m_bRuntimeProfilingEnabled = false;
	bool m_bRuntimeAdaptiveWorkBudgetsEnabled = false;
	uint64_t m_nRuntimeAdaptiveTargetUsec = 1000u;
	uint64_t m_nRuntimeAdaptiveHardMaxUsec = 2000u;
	int m_nRuntimeAdaptiveAdjustmentWindowFrames = 4;
	int m_nRuntimeAdaptiveMinGpuPages = 1;
	int m_nRuntimeAdaptiveMinCpuPages = 1;
	int m_nRuntimeAdaptiveMinInstancePages = 1;
	int m_nRuntimeAdaptiveMinInstancerCells = 4;
	int m_nRuntimeAdaptiveMinNodeLoadRegions = 1;
	int m_nRuntimeAdaptiveMinNodeUnloadRegions = 1;
	uint64_t m_nRuntimeAdaptiveLastMeasuredUsec = 0u;
	uint64_t m_nRuntimeAdaptiveControllerUsecLast = 0u;
	t3dAdaptiveBudgetState_t m_nRuntimeAdaptiveBudgetState;
	bool m_bRuntimeBoundaryWorkPhasingEnabled = true;
	int m_nRuntimeBoundaryNodeDelayFrames = 1;
	int m_nRuntimeBoundaryInstancerDelayFrames = 2;
	uint64_t m_iRuntimePhysicsFrameIndex = 0u;
	uint64_t m_nRuntimeBoundaryTransitionFrame = 0u;
	uint64_t m_nRuntimeBoundaryNodeDeferralsTotal = 0u;
	uint64_t m_nRuntimeBoundaryInstancerDeferralsTotal = 0u;
	int m_nRuntimeBoundaryNodeDeferralStreak = 0;
	int m_nRuntimeBoundaryInstancerDeferralStreak = 0;
	Vector2i m_runtimeBoundaryTargetRegion = s_v2iMax;
	bool m_bRuntimeBoundaryNodeDeferredLastTick = false;
	bool m_bRuntimeBoundaryInstancerDeferredLastTick = false;
	bool m_bGpuRegionCacheEnabled = false;
	bool m_bDeferGpuRegionCacheReconfigure = false;
	int m_nGpuRegionCacheBudgetMb = 16;
	int m_nGpuRegionUploadBudgetLayers = 4;
	int m_nGpuRegionUploadBudgetMb = 2;
	int m_nGpuRegionSparePages = s_nGpuCacheDefaultSpareLayers;
	int m_nGpuRegionRetireFrames = s_nGpuCacheDefaultRetireFrames;
	int m_nGpuRegionDetailLods = 0;
	int m_nGpuRegionGuardRegions = s_nGpuCacheDefaultGuardRegions;
	int m_nGpuRegionPrefetchRows = s_nGpuCacheDefaultPrefetchRows;
	int m_nGpuRegionHysteresisTicks = s_nGpuCacheDefaultHysteresisTicks;
	int m_nGpuRegionColorResolutionDivisor = 1;
	int m_nGpuRegionDerivedResolutionDivisor = 1;
	int m_nGpuMiddleHeightResolutionDivisor = 1;
	int m_nEditorGpuRegionCachePages = 96;
	int m_nGpuFarQualityDivisor = 1;
	int m_nFarMaterialResolution = 128;
	int m_nFarControlResolution = 128;
	bool m_bGpuRegionCookedPagesEnabled = false;
	bool m_bGpuRegionCookOnSave = false;
	bool m_bRuntimeMaterialStreamingEnabled = false;
	int m_nRuntimeMaterialCacheLayers = 12;
	int m_nRuntimeMaterialUploadBudgetLayers = 2;
	bool m_bRuntimeReleaseSourceMaterialTextures = false;
	bool m_bCpuRegionCacheEnabled = false;
	int m_nCpuRegionCacheBudgetMb = 16;
	int m_nCpuRegionLoadBudgetLayers = 4;
	int m_nCpuRegionLoadBudgetMb = 2;
	bool m_bInstanceRegionCacheEnabled = false;
	int m_nInstanceRegionCacheBudgetMb = 16;
	int m_nInstanceRegionLoadBudgetPages = 4;
	int m_nInstanceRegionLoadBudgetMb = 2;
	int m_nInstancerCellSize = s_nInstanceDefaultCellSize;
	bool m_bInstancerCellStreamingEnabled = true;
	bool m_bInstancerSingleLodBuffers = true;
	real_t m_fInstancerCellStreamingMargin = 64.0f;
	real_t m_fInstancerCellStreamingUpdateDistance = 16.0f;
	int m_nInstancerUpdateCellBudget = 32;
	bool m_bDynamicNodeStreamingEnabled = true;
	int m_nDynamicNodeStreamRadius = 1;
	int m_nDynamicNodeLoadRegionBudget = 2;
	int m_nDynamicNodeLoadNodeBudget = 16;
	int m_nDynamicNodeUnloadRegionBudget = 4;
	bool m_bLightmapStreamingEnabled = true;
	int m_nLightmapStreamRadius = 1;
	int m_nLightmapLoadBudget = 2;
	int m_nLightmapUnloadBudget = 4;
	String m_regionLightmapOutputDirectory;
	int m_nRegionLightmapProxyLod = 2;
	int m_nRegionLightmapContextRadius = 1;
	real_t m_fRegionLightmapTexelSize = 1.0f;
	int m_nRegionLightmapBakeQuality = 2;
	int m_nRegionLightmapShadowmaskMode = 2;
	bool m_bHlodEnabled = true;
	String m_hlodManifestPath;
	real_t m_fHlodTargetErrorPixels = 3.0f;
	real_t m_fHlodHysteresis = 0.20f;
	int m_nHlodMaxLevel = 6;
	int m_nHlodLoadBudget = 2;
	int m_nHlodUnloadBudget = 4;
	int m_nHlodMemoryBudgetMb = 64;
	int m_nHlodMaxShadowLevel = 3;
	int m_nHlodForcedDebugLevel = -1;

	// Total-memory budget policy. Manual subsystem budgets are retained as overrides;
	// automatic mode derives effective budgets at the low-frequency policy cadence.
	int m_nTotalMemoryBudgetMb = 128;
	int m_nMemoryTransientReserveMb = 16;
	bool m_bGpuRegionCacheBudgetAuto = true;
	bool m_bCpuRegionCacheBudgetAuto = true;
	bool m_bInstanceRegionCacheBudgetAuto = true;
	bool m_bLightmapResidencyBudgetAuto = true;
	bool m_bHlodMemoryBudgetAuto = true;
	bool m_bShaderCacheBudgetAuto = true;
	int m_nLightmapResidencyBudgetMb = 16;
	int m_nEffectiveGpuRegionCacheBudgetMb = 16;
	int m_nEffectiveCpuRegionCacheBudgetMb = 16;
	int m_nEffectiveInstanceRegionCacheBudgetMb = 16;
	int m_nEffectiveLightmapResidencyBudgetMb = 16;
	int m_nEffectiveHlodMemoryBudgetMb = 16;
	int m_nEffectiveShaderCacheBudgetMb = 8;
	bool m_bMemoryBudgetPolicyDirty = true;
	uint64_t m_nMemoryBudgetPolicyFrame = 0u;
	uint64_t m_nMemoryBudgetPolicyGeneration = 0u;
	mutable uint64_t m_nTrackedMemoryPeakBytes = 0u;
	t3dMemoryBudgetResult_t m_nMemoryBudgetResult;
	mutable uint64_t m_nDiagnosticsCollectionUsecLast = 0u;
	mutable uint64_t m_nDiagnosticsCollectionUsecTotal = 0u;
	mutable uint64_t m_nDiagnosticsCollectionUsecPeak = 0u;
	mutable uint64_t m_nDiagnosticsCollectionCount = 0u;
	mutable eastl::array<uint64_t, 7> m_szDiagnosticCachePeakBytes{};
	bool m_bMemoryPressureEventState = false;
	bool m_bMemoryOvercommitEventState = false;
	eastl::array<bool, 3> m_bCacheThrashEventState{};
	int m_nNextStreamOwnerId = 0x40000000;
	real_t m_fLabelDistance = 0.f;
	int m_nLabelSize = 48;

	// Tracked Targets
	Terrain3DTargetNode3D m_clipmapTarget;
	Terrain3DTargetNode3D m_collisionTarget;
	Terrain3DTargetNode3D m_lightTarget;
	Terrain3DTargetNode3D m_hCamera; // Fallback target for clipmap and collision
	Color m_lastLightColor = s_colorWhite;
	Vector3 m_lastLightDirection = s_v3Zero;
	bool m_bLightStateValid = false;
	bool m_bSemanticShadowPolicyEnabled = false;
	t3dEnvironmentShadowQuality_t m_environmentShadowQuality = ENVIRONMENT_SHADOW_NORMAL;
	uint32_t m_nEnvironmentShadowExternalMask = 1u;
	uint32_t m_nLightShadowCasterMaskBeforeSemantic = 0xFFFFFu;
	bool m_bLightShadowMaskCaptured = false;
	bool m_bSemanticDirectionalReceptionEnabled = false;
	uint32_t m_nEnvironmentDirectionalExternalMask = 0u;
	uint32_t m_nLightCullMaskBeforeSemantic = 0xFFFFFu;
	bool m_bLightCullMaskCaptured = false;

	// Terrain Mesh
	Terrain3DMesher *m_pTerrainMesher = nullptr;
	Ref<Terrain3DMaterial> m_material;
	int m_nMeshLods = 7;
	int m_nTessellationLevel = 0;
	int m_nMeshSize = 48;
	real_t m_fVertexSpacing = 1.0f;
	real_t m_fCullMargin = 0.0f;
	RenderingServer::ShadowCastingSetting m_castShadows = RenderingServer::SHADOW_CASTING_SETTING_ON;
	int m_nShadowLodCount = 2;
	int m_nNearShadowLodCount = -1;
	int m_nMiddleShadowLodCount = -1;
	int m_nFarShadowLodCount = -1;
	bool m_bNearShadowEnabled = true;
	bool m_bMiddleShadowEnabled = true;
	bool m_bFarShadowEnabled = true;
	bool m_bRuntimeShaderTieringEnabled = false;
	int m_nRuntimeNearShaderLods = 2;
	int m_nRuntimeMiddleShaderLods = 3;
	int m_nRuntimeClipmapUpdateBudget = 2;
	int m_nRuntimePagePrepareThreads = 2;
	bool m_bRuntimeSyncDetailLods = true;
	GeometryInstance3D::GIMode m_giMode = GeometryInstance3D::GI_MODE_STATIC;
	uint32_t m_nRenderLayers = 1u | ( 1u << 31u ); // Bit 1 and 32 for the cursor

	// Displacement Buffer
	SubViewport *m_pDBufferVp = nullptr;
	ColorRect *m_pDBufferRect = nullptr;
	Vector2 m_lastBufferPosition = s_v2Max;
	std::atomic_bool m_bComputeDisplacementEnabled{ false };
	std::atomic_bool m_bComputeDisplacementActive{ false };
	std::atomic_bool m_bComputeDisplacementDispatchPending{ false };
	std::atomic_bool m_bComputeDisplacementFailed{ false };
	std::atomic_bool m_bComputeDisplacementFallbackRequested{ false };
	std::atomic_bool m_bComputeDisplacementTearingDown{ false };
	real_t m_fDisplacementUpdateDistance = 0.25f;
	mutable std::mutex m_computeDisplacementMutex;
	t3dDisplacementRequest_t m_computeDisplacementRequest;
	RID m_hComputeDisplacementTextureRd;
	uint64_t m_nComputeDisplacementRequestVersion = 0u;

	// Ocean Mesh
	Terrain3DMesher *m_pOceanMesher = nullptr;
	bool m_bOceanEnabled = false;
	int m_nOceanMeshLods = 7;
	int m_nOceanTessellationLevel = 0;
	int m_nOceanMeshSize = 32;
	real_t m_fOceanVertexSpacing = 4.0f;
	real_t m_fOceanCullMargin = 20.0f;
	RenderingServer::ShadowCastingSetting m_oceanCastShadows = RenderingServer::SHADOW_CASTING_SETTING_OFF;
	GeometryInstance3D::GIMode m_oceanGiMode = GeometryInstance3D::GI_MODE_DISABLED;
	uint32_t m_nOceanRenderLayers = 1u;
	Ref<Material> m_oceanMaterial;
	Ref<Shader> m_oceanVariantShader;
	String m_oceanVariantSource;
	t3dOceanPerformancePreset_t m_oceanPerformancePreset = OCEAN_PRESET_BORDER_BALANCED;
	int m_nOceanVertexWaveIterations = 1;
	int m_nOceanFragmentWaveIterations = 1;
	t3dOceanRefractionMode_t m_oceanRefractionMode = OCEAN_REFRACTION_DEPTH_TINT;
	t3dOceanFoamMode_t m_oceanFoamMode = OCEAN_FOAM_SHORE;
	t3dOceanScatteringMode_t m_oceanScatteringMode = OCEAN_SCATTERING_APPROXIMATE;
	t3dOceanShadingMode_t m_oceanShadingMode = OCEAN_SHADING_LAMBERT_GGX;

	// Rendering
	Terrain3DRenderCoordinator *m_pRenderCoordinator = nullptr;
	t3dRenderBackend_t m_renderBackend = T3D_RENDER_BACKEND_RENDERING_SERVER;
	t3dRdNativeProxyMode_t m_rdNativeProxyMode = T3D_RD_NATIVE_PROXY_SHADOWS_ONLY;
	bool m_bFreeEditorTextures = true;
	// Mouse cursor
	SubViewport *m_pMouseVp = nullptr;
	Camera3D *m_pMouseCam = nullptr;
	MeshInstance3D *m_pMouseQuad = nullptr;
	uint32_t m_nMouseLayer = 32u;
	// Parent containers for child nodes
	Node3D *m_pLabelParent = nullptr;

	void initializeInternal( void );
	bool initializeRuntimeMemory( void );
	void deactivateRuntime( void );
	bool isEditorAuthoringModeInternal( void ) const;
	bool effectiveGpuRegionCacheEnabled( void ) const;
	bool effectiveCpuRegionCacheEnabled( void ) const;
	bool effectiveInstanceRegionCacheEnabled( void ) const;
	bool effectiveInstancerCellStreamingEnabled( void ) const;
	bool effectiveDynamicNodeStreamingEnabled( void ) const;
	void applyStreamingPolicy( bool bForceResidency = false );
	void updateRegionStreamingPolicy( const Vector3 &rWorldPosition, double fDelta, bool bForce = false );
	void invalidateRegionStreamingPolicy( void );
	void validateRegionStreamingConfiguration( bool bReport = true ) const;
	void markMemoryBudgetDirty( void );
	void serviceMemoryBudgetPolicy( bool bForce );
	t3dMemoryBudgetInput_t buildMemoryBudgetInput( void ) const;
	uint64_t calculateFixedMemoryBytes( void ) const;
	uint64_t calculateTransientMemoryBytes( void ) const;
	size_t calculateRuntimeAllocatorZoneBytes( void ) const;
	void applyMemoryBudgetResult( const t3dMemoryBudgetResult_t &rResult, bool bForce );
	static const char *memoryLimitReasonName( t3dMemoryLimitReason_t reason );
	void sampleRuntimeProfile( const t3dRuntimeProfileSample_t &rSample );
	void updateCacheThrashEvents( const t3dRuntimeProfileSample_t &rSample );
	Dictionary getRuntimeMemoryStatsFast( void ) const;
	Array getShaderTierStatsDictionaries( void ) const;
	Array getCacheInventory( bool bDeep ) const;
	Dictionary getCpuProfileStats( void ) const;
	Dictionary getGpuInformationStats( void ) const;
	Dictionary runtimeProfileSampleToDictionary( const t3dRuntimeProfileSample_t &rSample ) const;
	Array getRuntimeProfileEventNames( uint32_t nFlags ) const;
	Array getRuntimeProfileStallNames( uint32_t nFlags ) const;
	Array getRuntimeProfileProblems( void ) const;
	Dictionary getRuntimeProfileAggregate( void ) const;
	t3dAdaptiveBudgetLimits_t getRuntimeAdaptiveBudgetLimits( void ) const;
	t3dAdaptiveBudgetPressure_t getRuntimeAdaptiveBudgetPressure( void ) const;
	void updateRuntimeAdaptiveWorkBudgets( void );
	void applyRuntimeAdaptiveWorkBudgets( void );
	void resetRuntimeAdaptiveWorkBudgets( bool bClearOverrides );
	String getRuntimeAdaptiveReasonName( void ) const;
	void refreshPerformanceProfileConformity( void );
	void onProfileOwnedResourceChanged( void );
	void applyPerformanceProfileToTerrain( void );
	void normalizeRuntimeTierLayout( bool bSyncDetailLods );
	void applyRuntimeTierLayout( bool bForceFullSnap = true );
	int getTotalClipmapLevels( void ) const;
	int rangeToLodCount( real_t fRangeM ) const;
	real_t lodCountToRange( int nLodCount ) const;
	void connectResourceSignals( const Ref<Terrain3DAssets> &rAssets, const Ref<Terrain3DMaterial> &rMaterial );
	void disconnectResourceSignals( const Ref<Terrain3DAssets> &rAssets, const Ref<Terrain3DMaterial> &rMaterial );
	void connectAutoShaderProfileSignal( const Ref<Resource> &rProfile );
	void disconnectAutoShaderProfileSignal( const Ref<Resource> &rProfile );
	void applyAutoShaderProfileMode( void );
	void onAutoShaderProfileChanged( void );
	void onDataMapsChanged( void );
	void onGpuRegionPageTableChanged( void );
	void onAssetsTexturesChanged( void );
	void physicsProcess( const double fDelta );
	void grabCamera( void );
	void syncLightShaderParameters( void );

	void destroyCollision( const bool bFinal = false );

	void setupTerrainMesher( void );
	void setupRenderCoordinator( void );
	void destroyRenderCoordinator( bool bFinal = false );
	EA_FORCE_INLINE void updateMesherAabbs( void ) {
		m_pTerrainMesher ? m_pTerrainMesher->updateAabbs() : ( void )0;
	}
	void destroyTerrainMesher( const bool bFinal = false );
	void setupOceanMesher( void );
	void updateOceanShaderVariant( void );
	EA_FORCE_INLINE void updateOceanAabbs( void ) {
		m_pOceanMesher ? m_pOceanMesher->updateAabbs() : ( void )0;
	}
	void destroyOceanMesher( const bool bFinal = false );

	void setupDisplacementBuffer( void );
	void updateDisplacementBuffer( void );
	bool tryUpdateComputeDisplacement( const Vector3 &rTargetPosition );
	void invalidateComputeDisplacementRequest( void );
	void queueComputeDisplacementRender( void );
	void renderUpdateComputeDisplacement( void );
	void renderShutdownComputeDisplacement( void );
	void destroyDisplacementBuffer( void );

	void buildContainers( void );
	void destroyContainers( void );
	void destroyLabels( void );
	void updateEnvironmentShadowCasterMask( void );
	void updateEnvironmentDirectionalReceiverMask( void );

	void setupMousePicking( void );
	void destroyMousePicking( void );
	void destroyInstancer( void );
	void destroyNodeStreamer( bool bCaptureState = true );
	void destroyLightmapStreamer( void );
	void destroyHlodStreamer( void );

	static bool bakeRegionSnapshotLess( const t3dBakeRegionSnapshot_t &rA, const t3dBakeRegionSnapshot_t &rB );
	static void runBakeTaskRange( void *pContext, size_t nBegin, size_t nEnd );
	Vector3 getTriangleVertex( const t3dBakeSnapshot_t *pSnapshot,
			int32_t nLod,
			t3dHeightFilter_t filter,
			int32_t iX,
			int32_t iZ,
			const Vector3 &rPosition,
			uint32_t &nControl ) const;

	t3dBakeSnapshot_t createBakeSnapshot( void ) const;
	void generateTriangles( PackedVector3Array &arrVertices,
			PackedVector2Array *pUvs,
			const int32_t nLod,
			const t3dHeightFilter_t filter,
			const bool require_nav,
			const AABB &rGlobalAabb ) const;
	void generateTrianglePair( t3dTriangleBuffer_t &rBuffer,
			const bool bGenerateUvs,
			const int32_t nLod,
			const t3dHeightFilter_t filter,
			const bool require_nav,
			const int32_t x,
			const int32_t z,
			const t3dBakeSnapshot_t *pSnapshot = nullptr ) const;

public:
	static constexpr int64_t s_nNativeApiRevision = 2026082203LL;
	static constexpr const char *s_pszNativeBuildId = "a2.9.2-shader-preprocessor-repair";
	static t3dDebugLevel_t s_debugLevel; // Initialized in terrain_3d.cpp

	Terrain3D( void );
	~Terrain3D( void );
	EA_FORCE_INLINE bool isInsideWorld( void ) const {
		return m_bIsInsideWorld;
	}
	EA_FORCE_INLINE bool isInitialized( void ) const {
		return m_bInitialized;
	}

	// Terrain
	EA_FORCE_INLINE String getVersion( void ) const {
		return m_version;
	}
	EA_FORCE_INLINE int64_t getNativeApiRevision( void ) const {
		return s_nNativeApiRevision;
	}
	EA_FORCE_INLINE String getNativeBuildId( void ) const {
		return String( s_pszNativeBuildId );
	}
	void setDebugLevel( const t3dDebugLevel_t level );
	EA_FORCE_INLINE t3dDebugLevel_t getDebugLevel( void ) const {
		return s_debugLevel;
	}
	void setDataDirectory( String dir );
	EA_FORCE_INLINE String getDataDirectory( void ) const {
		return m_pData ? m_dataDirectory : "";
	}
	bool cookRuntimeClipmapGeometryPack( void ) const;

	// Object references
	EA_FORCE_INLINE Terrain3DData *getData( void ) const {
		return m_pData;
	}
	void setAssets( const Ref<Terrain3DAssets> &rAssets );
	EA_FORCE_INLINE Ref<Terrain3DAssets> getAssets( void ) const {
		return m_assets;
	}
	void setAutoShaderProfile( const Ref<Resource> &rProfile );
	EA_FORCE_INLINE Ref<Resource> getAutoShaderProfile( void ) const {
		return m_autoShaderProfile;
	}
	EA_FORCE_INLINE Terrain3DCollision *getCollision( void ) const {
		return m_pCollision;
	}
	EA_FORCE_INLINE Terrain3DInstancer *getInstancer( void ) const {
		return m_pInstancer;
	}
	EA_FORCE_INLINE Terrain3DNodeStreamer *getNodeStreamer( void ) const {
		return m_pNodeStreamer;
	}
	EA_FORCE_INLINE Terrain3DLightmapStreamer *getLightmapStreamer( void ) const {
		return m_pLightmapStreamer;
	}
	EA_FORCE_INLINE Terrain3DHLODStreamer *getHlodStreamer( void ) const {
		return m_pHlodStreamer;
	}
	void setEditor( Terrain3DEditor *pEditor );
	EA_FORCE_INLINE Terrain3DEditor *getEditor( void ) const {
		return m_pEditor;
	}
	void setPlugin( Object *pPlugin );
	EA_FORCE_INLINE Object *getPlugin( void ) const {
		return m_pEditorPlugin;
	}

	// Regions
	void setRegionSize( const t3dRegionSize_t size );
	EA_FORCE_INLINE t3dRegionSize_t getRegionSize( void ) const {
		return m_nRegionSize;
	}
	void changeRegionSize( const t3dRegionSize_t size ) {
		if ( m_pData && m_pData->isInitializedFor( this ) ) {
			m_pData->changeRegionSize( size );
		} else {
			// Property deserialization can create Terrain3DData before the node enters
			// the tree/world and before Terrain3DData::initialize() assigns its owner.
			// At that stage there is no repository to migrate; retain the requested
			// owner setting and let Terrain3DData::initialize() reconcile storage later.
			setRegionSize( size );
		}
	}
	void setSave16Bit( const bool bEnabled );
	EA_FORCE_INLINE bool getSave16Bit( void ) const {
		return m_bSave16Bit;
	}
	void setPerformanceTarget( const t3dPerformanceTarget_t target );
	EA_FORCE_INLINE t3dPerformanceTarget_t getPerformanceTarget( void ) const {
		return m_performanceTarget;
	}
	EA_FORCE_INLINE t3dPerformanceProfileStatus_t getPerformanceProfileStatus( void ) const {
		return m_performanceProfileStatus;
	}
	String getPerformanceProfileStatusName( void ) const;
	int getPerformanceProfileVersion( void ) const;
	String getPerformanceProfileSignature( void ) const;
	Dictionary getPerformanceProfileManifest( void ) const;
	Array getPerformanceProfileDiff( void ) const;
	void resetPerformanceProfile( void );
	void setEditorExecutionMode( t3dEditorExecutionMode_t mode );
	EA_FORCE_INLINE t3dEditorExecutionMode_t getEditorExecutionMode( void ) const {
		return m_editorExecutionMode;
	}
	void setEditorRuntimeStreamingPreview( bool bEnabled );
	EA_FORCE_INLINE bool isEditorRuntimeStreamingPreview( void ) const {
		return m_editorExecutionMode == EDITOR_EXECUTION_RUNTIME_PROFILE;
	}
	void setEditorNodeStreamingPreview( bool bEnabled );
	EA_FORCE_INLINE bool isEditorNodeStreamingPreviewEnabled( void ) const {
		return m_bEditorNodeStreamingPreview;
	}
	EA_FORCE_INLINE bool isEditorNodeStreamingPreviewActive( void ) const {
		return isEditorAuthoringModeInternal() && m_bEditorNodeStreamingPreview;
	}
	EA_FORCE_INLINE bool isEditorAuthoringMode( void ) const {
		return isEditorAuthoringModeInternal();
	}
	bool isAuthoringResidencyReadbackSafe( void ) const;
	EA_FORCE_INLINE bool isGpuRegionCacheEffective( void ) const {
		return effectiveGpuRegionCacheEnabled();
	}
	EA_FORCE_INLINE bool isCpuRegionCacheEffective( void ) const {
		return effectiveCpuRegionCacheEnabled();
	}
	EA_FORCE_INLINE bool isInstanceRegionCacheEffective( void ) const {
		return effectiveInstanceRegionCacheEnabled();
	}
	EA_FORCE_INLINE bool isInstancerCellStreamingEffective( void ) const {
		return effectiveInstancerCellStreamingEnabled();
	}
	EA_FORCE_INLINE bool isDynamicNodeStreamingEffective( void ) const {
		return effectiveDynamicNodeStreamingEnabled();
	}
	void setStreamingActiveRadius( int nRadius );
	EA_FORCE_INLINE int getStreamingActiveRadius( void ) const {
		return m_regionStreamingConfig.m_nActiveRadius;
	}
	void setStreamingHotRadius( int nRadius );
	EA_FORCE_INLINE int getStreamingHotRadius( void ) const {
		return m_regionStreamingConfig.m_nHotRadius;
	}
	void setStreamingColdRadius( int nRadius );
	EA_FORCE_INLINE int getStreamingColdRadius( void ) const {
		return m_regionStreamingConfig.m_nColdRadius;
	}
	void setStreamingUnloadGuard( int nRegions );
	EA_FORCE_INLINE int getStreamingUnloadGuard( void ) const {
		return m_regionStreamingConfig.m_nUnloadGuard;
	}
	void setStreamingPredictionEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isStreamingPredictionEnabled( void ) const {
		return m_regionStreamingConfig.m_bPredictionEnabled;
	}
	void setStreamingPredictionLookahead( real_t fSeconds );
	EA_FORCE_INLINE real_t getStreamingPredictionLookahead( void ) const {
		return ( real_t )( m_regionStreamingConfig.m_fPredictionLookaheadSeconds );
	}
	void setStreamingPredictionMaximumRegions( int nRegions );
	EA_FORCE_INLINE int getStreamingPredictionMaximumRegions( void ) const {
		return m_regionStreamingConfig.m_nPredictionMaximumRegions;
	}
	t3dRegionStreamingState_t getRegionStreamingStateInternal( const Vector2i &vLocation ) const;
	bool isRegionStreamingPrefetchInternal( const Vector2i &vLocation ) const;
	EA_FORCE_INLINE const uint8_t *getRegionStreamingStatesInternal( void ) const {
		return m_szRegionStreamingStates;
	}
	EA_FORCE_INLINE const uint8_t *getRegionStreamingPrefetchInternal( void ) const {
		return m_szRegionStreamingPrefetch;
	}
	EA_FORCE_INLINE int getRegionStreamingState( const Vector2i &vLocation ) const {
		return ( int )( getRegionStreamingStateInternal( vLocation ) );
	}
	EA_FORCE_INLINE bool isRegionStreamingPrefetch( const Vector2i &vLocation ) const {
		return isRegionStreamingPrefetchInternal( vLocation );
	}
	Dictionary getRegionStreamingStats( void ) const;
	Dictionary getRegionStreamingConfigurationStatus( void ) const;
	Dictionary getHeightSourceDiagnostics( const Vector3 &rWorldPosition ) const;
	EA_FORCE_INLINE uint64_t getRegionStreamingGeneration( void ) const {
		return m_nRegionStreamingGeneration;
	}
	EA_FORCE_INLINE Vector2i getRegionStreamingCenterInternal( void ) const {
		return m_regionStreamingCenter;
	}
	EA_FORCE_INLINE Vector2i getRegionStreamingPredictedCenterInternal( void ) const {
		return m_regionStreamingPredictedCenter;
	}

	bool isRuntimeShaderTieringActive( void ) const {
		// Authoring deliberately keeps the complete Near shader on every clipmap level.
		// Runtime Profile and exported runtime exercise the compact Near/Middle/Far layout.
		return m_bRuntimeShaderTieringEnabled && !isEditorAuthoringModeInternal();
	}
	Dictionary getMemoryEstimate( void ) const;
	Dictionary getTrackedCurrentResidency( void ) const;
	Dictionary getMemoryBudgetStats( void ) const;
	void setTotalMemoryBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getTotalMemoryBudgetMb( void ) const {
		return m_nTotalMemoryBudgetMb;
	}
	void setMemoryTransientReserveMb( int nReserveMb );
	EA_FORCE_INLINE int getMemoryTransientReserveMb( void ) const {
		return m_nMemoryTransientReserveMb;
	}
	void setGpuRegionCacheBudgetAuto( bool bEnabled );
	EA_FORCE_INLINE bool isGpuRegionCacheBudgetAuto( void ) const {
		return m_bGpuRegionCacheBudgetAuto;
	}
	void setCpuRegionCacheBudgetAuto( bool bEnabled );
	EA_FORCE_INLINE bool isCpuRegionCacheBudgetAuto( void ) const {
		return m_bCpuRegionCacheBudgetAuto;
	}
	void setInstanceRegionCacheBudgetAuto( bool bEnabled );
	EA_FORCE_INLINE bool isInstanceRegionCacheBudgetAuto( void ) const {
		return m_bInstanceRegionCacheBudgetAuto;
	}
	void setLightmapResidencyBudgetAuto( bool bEnabled );
	EA_FORCE_INLINE bool isLightmapResidencyBudgetAuto( void ) const {
		return m_bLightmapResidencyBudgetAuto;
	}
	void setLightmapResidencyBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getLightmapResidencyBudgetMb( void ) const {
		return m_nLightmapResidencyBudgetMb;
	}
	void setHlodMemoryBudgetAuto( bool bEnabled );
	EA_FORCE_INLINE bool isHlodMemoryBudgetAuto( void ) const {
		return m_bHlodMemoryBudgetAuto;
	}
	void setShaderCacheBudgetAuto( bool bEnabled );
	EA_FORCE_INLINE bool isShaderCacheBudgetAuto( void ) const {
		return m_bShaderCacheBudgetAuto;
	}
	EA_FORCE_INLINE int getEffectiveGpuRegionCacheBudgetMb( void ) const {
		return effectiveGpuRegionCacheEnabled() ? m_nEffectiveGpuRegionCacheBudgetMb : 0;
	}
	EA_FORCE_INLINE int getEffectiveCpuRegionCacheBudgetMb( void ) const {
		return effectiveCpuRegionCacheEnabled() ? m_nEffectiveCpuRegionCacheBudgetMb : 0;
	}
	EA_FORCE_INLINE int getEffectiveInstanceRegionCacheBudgetMb( void ) const {
		return effectiveInstanceRegionCacheEnabled() ? m_nEffectiveInstanceRegionCacheBudgetMb : 0;
	}
	EA_FORCE_INLINE int getEffectiveLightmapResidencyBudgetMb( void ) const {
		return m_bLightmapStreamingEnabled ? m_nEffectiveLightmapResidencyBudgetMb : 0;
	}
	EA_FORCE_INLINE int getEffectiveHlodMemoryBudgetMb( void ) const {
		return m_bHlodEnabled ? m_nEffectiveHlodMemoryBudgetMb : 0;
	}
	EA_FORCE_INLINE int getEffectiveShaderCacheBudgetMb( void ) const {
		return m_nEffectiveShaderCacheBudgetMb;
	}
	EA_FORCE_INLINE int64_t getMemoryBudgetPolicyGeneration( void ) const {
		return ( int64_t )( m_nMemoryBudgetPolicyGeneration );
	}
	EA_FORCE_INLINE bool isMemoryBudgetOvercommitted( void ) const {
		return m_nMemoryBudgetResult.m_bHardOvercommit;
	}
	EA_FORCE_INLINE int64_t getMemoryBudgetHeadroomBytes( void ) const {
		return ( int64_t )( m_nMemoryBudgetResult.m_nHeadroomBytes );
	}
	void forceMemoryBudgetRebalance( void );
	Dictionary getRuntimeDiagnostics( bool bDeep = false ) const;
	Dictionary getRuntimeParitySnapshot( void ) const;
	Ref<Terrain3DMemoryStats> getMemoryStatsSnapshot( void ) const;
	TypedArray<Terrain3DShaderTierStats> getShaderTierStatsSnapshot( void ) const;
	TypedArray<Terrain3DCacheStats> getCacheStatsSnapshot( bool bDeep = false ) const;
	Ref<Terrain3DFrameStats> getFrameStatsSnapshot( void ) const;
	Array getRuntimeProfileHistory( int nMaxSamples = 600 ) const;
	Array getRuntimeProfileHistorySince( int64_t nAfterSequence, int nMaxSamples = 600 ) const;
	void resetRuntimeDiagnostics( void );
	void setRuntimeProfilingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isRuntimeProfilingEnabled( void ) const {
		return m_bRuntimeProfilingEnabled;
	}
	void setRuntimeAdaptiveWorkBudgetsEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isRuntimeAdaptiveWorkBudgetsEnabled( void ) const {
		return m_bRuntimeAdaptiveWorkBudgetsEnabled;
	}
	void setRuntimeAdaptiveTargetMs( real_t fTargetMs );
	EA_FORCE_INLINE real_t getRuntimeAdaptiveTargetMs( void ) const {
		return ( real_t )( m_nRuntimeAdaptiveTargetUsec ) / 1000.0f;
	}
	void setRuntimeAdaptiveHardMaxMs( real_t fHardMaxMs );
	EA_FORCE_INLINE real_t getRuntimeAdaptiveHardMaxMs( void ) const {
		return ( real_t )( m_nRuntimeAdaptiveHardMaxUsec ) / 1000.0f;
	}
	void setRuntimeAdaptiveAdjustmentWindowFrames( int nFrames );
	EA_FORCE_INLINE int getRuntimeAdaptiveAdjustmentWindowFrames( void ) const {
		return m_nRuntimeAdaptiveAdjustmentWindowFrames;
	}
	void setRuntimeAdaptiveMinGpuPages( int nPages );
	EA_FORCE_INLINE int getRuntimeAdaptiveMinGpuPages( void ) const {
		return m_nRuntimeAdaptiveMinGpuPages;
	}
	void setRuntimeAdaptiveMinCpuPages( int nPages );
	EA_FORCE_INLINE int getRuntimeAdaptiveMinCpuPages( void ) const {
		return m_nRuntimeAdaptiveMinCpuPages;
	}
	void setRuntimeAdaptiveMinInstancePages( int nPages );
	EA_FORCE_INLINE int getRuntimeAdaptiveMinInstancePages( void ) const {
		return m_nRuntimeAdaptiveMinInstancePages;
	}
	void setRuntimeAdaptiveMinInstancerCells( int nCells );
	EA_FORCE_INLINE int getRuntimeAdaptiveMinInstancerCells( void ) const {
		return m_nRuntimeAdaptiveMinInstancerCells;
	}
	void setRuntimeAdaptiveMinNodeLoadRegions( int nRegions );
	EA_FORCE_INLINE int getRuntimeAdaptiveMinNodeLoadRegions( void ) const {
		return m_nRuntimeAdaptiveMinNodeLoadRegions;
	}
	void setRuntimeAdaptiveMinNodeUnloadRegions( int nRegions );
	EA_FORCE_INLINE int getRuntimeAdaptiveMinNodeUnloadRegions( void ) const {
		return m_nRuntimeAdaptiveMinNodeUnloadRegions;
	}
	Dictionary getRuntimeAdaptiveBudgetStats( void ) const;
	void setRuntimeBoundaryWorkPhasingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isRuntimeBoundaryWorkPhasingEnabled( void ) const {
		return m_bRuntimeBoundaryWorkPhasingEnabled;
	}
	void setRuntimeBoundaryNodeDelayFrames( int nFrames );
	EA_FORCE_INLINE int getRuntimeBoundaryNodeDelayFrames( void ) const {
		return m_nRuntimeBoundaryNodeDelayFrames;
	}
	void setRuntimeBoundaryInstancerDelayFrames( int nFrames );
	EA_FORCE_INLINE int getRuntimeBoundaryInstancerDelayFrames( void ) const {
		return m_nRuntimeBoundaryInstancerDelayFrames;
	}
	Dictionary getRuntimeBoundaryPhasingStats( void ) const;
	void setGpuRegionCacheEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isGpuRegionCacheEnabled( void ) const {
		return m_bGpuRegionCacheEnabled;
	}
	void setGpuRegionCacheBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getGpuRegionCacheBudgetMb( void ) const {
		return m_nGpuRegionCacheBudgetMb;
	}
	void setGpuRegionUploadBudgetLayers( int nLayers );
	EA_FORCE_INLINE int getGpuRegionUploadBudgetLayers( void ) const {
		return m_nGpuRegionUploadBudgetLayers;
	}
	void setGpuRegionUploadBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getGpuRegionUploadBudgetMb( void ) const {
		return m_nGpuRegionUploadBudgetMb;
	}
	void setGpuRegionSparePages( int nPages );
	EA_FORCE_INLINE int getGpuRegionSparePages( void ) const {
		return m_nGpuRegionSparePages;
	}
	void setGpuRegionRetireFrames( int nFrames );
	EA_FORCE_INLINE int getGpuRegionRetireFrames( void ) const {
		return m_nGpuRegionRetireFrames;
	}
	void setGpuRegionDetailLods( int nLods );
	EA_FORCE_INLINE int getGpuRegionDetailLods( void ) const {
		return m_nGpuRegionDetailLods;
	}
	void setGpuRegionDetailRangeEndM( real_t fRangeM );
	real_t getGpuRegionDetailRangeEndM( void ) const;
	int getGpuRegionMaxSupportedDetailLods( void ) const;
	real_t getGpuRegionMaxSupportedDetailRangeM( void ) const;
	int getGpuRegionRequiredCorePages( void ) const;
	int getGpuRegionMappedCapacity( void ) const;
	bool isGpuRegionDetailRangeValid( void ) const;
	void setGpuRegionGuardRegions( int nRegions );
	EA_FORCE_INLINE int getGpuRegionGuardRegions( void ) const {
		return m_nGpuRegionGuardRegions;
	}
	void setGpuRegionPrefetchRows( int nRows );
	EA_FORCE_INLINE int getGpuRegionPrefetchRows( void ) const {
		return m_nGpuRegionPrefetchRows;
	}
	void setGpuRegionHysteresisTicks( int nTicks );
	EA_FORCE_INLINE int getGpuRegionHysteresisTicks( void ) const {
		return m_nGpuRegionHysteresisTicks;
	}
	void setGpuRegionColorResolutionDivisor( int nDivisor );
	EA_FORCE_INLINE int getGpuRegionColorResolutionDivisor( void ) const {
		return m_nGpuRegionColorResolutionDivisor;
	}
	void setGpuRegionDerivedResolutionDivisor( int nDivisor );
	EA_FORCE_INLINE int getGpuRegionDerivedResolutionDivisor( void ) const {
		return m_nGpuRegionDerivedResolutionDivisor;
	}
	void setGpuMiddleHeightResolutionDivisor( int nDivisor );
	EA_FORCE_INLINE int getGpuMiddleHeightResolutionDivisor( void ) const {
		return m_nGpuMiddleHeightResolutionDivisor;
	}
	void setEditorGpuRegionCachePages( int nPages );
	EA_FORCE_INLINE int getEditorGpuRegionCachePages( void ) const {
		return m_nEditorGpuRegionCachePages;
	}
	void setRuntimeMaterialStreamingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isRuntimeMaterialStreamingEnabled( void ) const {
		return m_bRuntimeMaterialStreamingEnabled;
	}
	void setRuntimeMaterialCacheLayers( int nLayers );
	EA_FORCE_INLINE int getRuntimeMaterialCacheLayers( void ) const {
		return m_nRuntimeMaterialCacheLayers;
	}
	void setRuntimeMaterialUploadBudgetLayers( int nLayers );
	EA_FORCE_INLINE int getRuntimeMaterialUploadBudgetLayers( void ) const {
		return m_nRuntimeMaterialUploadBudgetLayers;
	}
	void setRuntimeReleaseSourceMaterialTextures( bool bEnabled );
	EA_FORCE_INLINE bool isRuntimeReleaseSourceMaterialTextures( void ) const {
		return m_bRuntimeReleaseSourceMaterialTextures;
	}
	void setGpuFarQualityDivisor( int nDivisor );
	EA_FORCE_INLINE int getGpuFarQualityDivisor( void ) const {
		return m_nGpuFarQualityDivisor;
	}
	void setFarMaterialResolution( int nResolution );
	EA_FORCE_INLINE int getFarMaterialResolution( void ) const {
		return m_nFarMaterialResolution;
	}
	void setFarControlResolution( int nResolution );
	EA_FORCE_INLINE int getFarControlResolution( void ) const {
		return m_nFarControlResolution;
	}
	int getGpuFarHeightAtlasSide( void ) const;
	int getGpuFarSurfaceAtlasSide( void ) const;
	EA_FORCE_INLINE int getGpuFarAtlasSide( void ) const {
		return getGpuFarHeightAtlasSide();
	}
	uint64_t getGpuFarAtlasBytes( void ) const;
	void setGpuRegionCookedPagesEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isGpuRegionCookedPagesEnabled( void ) const {
		return m_bGpuRegionCookedPagesEnabled;
	}
	void setGpuRegionCookOnSave( bool bEnabled );
	EA_FORCE_INLINE bool isGpuRegionCookOnSave( void ) const {
		return m_bGpuRegionCookOnSave;
	}
	EA_FORCE_INLINE Dictionary getGpuRegionCacheStats( void ) const {
		return m_pData ? m_pData->getGpuRegionCacheStats() : Dictionary();
	}
	void setCpuRegionCacheEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isCpuRegionCacheEnabled( void ) const {
		return m_bCpuRegionCacheEnabled;
	}
	void setCpuRegionCacheBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getCpuRegionCacheBudgetMb( void ) const {
		return m_nCpuRegionCacheBudgetMb;
	}
	void setCpuRegionLoadBudgetLayers( int nLayers );
	EA_FORCE_INLINE int getCpuRegionLoadBudgetLayers( void ) const {
		return m_nCpuRegionLoadBudgetLayers;
	}
	void setCpuRegionLoadBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getCpuRegionLoadBudgetMb( void ) const {
		return m_nCpuRegionLoadBudgetMb;
	}
	EA_FORCE_INLINE Dictionary getCpuRegionCacheStats( void ) const {
		return m_pData ? m_pData->getCpuRegionCacheStats() : Dictionary();
	}
	void setInstanceRegionCacheEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isInstanceRegionCacheEnabled( void ) const {
		return m_bInstanceRegionCacheEnabled;
	}
	void setInstanceRegionCacheBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getInstanceRegionCacheBudgetMb( void ) const {
		return m_nInstanceRegionCacheBudgetMb;
	}
	void setInstanceRegionLoadBudgetPages( int nPages );
	EA_FORCE_INLINE int getInstanceRegionLoadBudgetPages( void ) const {
		return m_nInstanceRegionLoadBudgetPages;
	}
	void setInstanceRegionLoadBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getInstanceRegionLoadBudgetMb( void ) const {
		return m_nInstanceRegionLoadBudgetMb;
	}
	EA_FORCE_INLINE Dictionary getInstanceRegionCacheStats( void ) const {
		return m_pData ? m_pData->getInstanceRegionCacheStats() : Dictionary();
	}
	void setInstancerCellSize( int nCellSize );
	EA_FORCE_INLINE int getInstancerCellSize( void ) const {
		return m_nInstancerCellSize;
	}
	void setInstancerCellStreamingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isInstancerCellStreamingEnabled( void ) const {
		return m_bInstancerCellStreamingEnabled;
	}
	void setInstancerSingleLodBuffers( bool bEnabled );
	EA_FORCE_INLINE bool isInstancerSingleLodBuffers( void ) const {
		return m_bInstancerSingleLodBuffers;
	}
	void setInstancerCellStreamingMargin( real_t fMargin );
	EA_FORCE_INLINE real_t getInstancerCellStreamingMargin( void ) const {
		return m_fInstancerCellStreamingMargin;
	}
	void setInstancerCellStreamingUpdateDistance( real_t fDistance );
	EA_FORCE_INLINE real_t getInstancerCellStreamingUpdateDistance( void ) const {
		return m_fInstancerCellStreamingUpdateDistance;
	}
	void setInstancerUpdateCellBudget( int nBudget );
	EA_FORCE_INLINE int getInstancerUpdateCellBudget( void ) const {
		return m_nInstancerUpdateCellBudget;
	}
	void setDynamicNodeStreamingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isDynamicNodeStreamingEnabled( void ) const {
		return m_bDynamicNodeStreamingEnabled;
	}
	void setDynamicNodeStreamRadius( int nRadius );
	EA_FORCE_INLINE int getDynamicNodeStreamRadius( void ) const {
		return m_nDynamicNodeStreamRadius;
	}
	void setDynamicNodeLoadRegionBudget( int nBudget );
	EA_FORCE_INLINE int getDynamicNodeLoadRegionBudget( void ) const {
		return m_nDynamicNodeLoadRegionBudget;
	}
	void setDynamicNodeLoadNodeBudget( int nBudget );
	EA_FORCE_INLINE int getDynamicNodeLoadNodeBudget( void ) const {
		return m_nDynamicNodeLoadNodeBudget;
	}
	void setDynamicNodeUnloadRegionBudget( int nBudget );
	EA_FORCE_INLINE int getDynamicNodeUnloadRegionBudget( void ) const {
		return m_nDynamicNodeUnloadRegionBudget;
	}
	void setLightmapStreamingEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isLightmapStreamingEnabled( void ) const {
		return m_bLightmapStreamingEnabled;
	}
	void setLightmapStreamRadius( int nRadius );
	EA_FORCE_INLINE int getLightmapStreamRadius( void ) const {
		return m_nLightmapStreamRadius;
	}
	void setLightmapLoadBudget( int nBudget );
	EA_FORCE_INLINE int getLightmapLoadBudget( void ) const {
		return m_nLightmapLoadBudget;
	}
	void setLightmapUnloadBudget( int nBudget );
	EA_FORCE_INLINE int getLightmapUnloadBudget( void ) const {
		return m_nLightmapUnloadBudget;
	}
	Dictionary getLightmapStreamingStats( void ) const;
	void setRegionLightmapOutputDirectory( const String &rDirectory );
	EA_FORCE_INLINE String getRegionLightmapOutputDirectory( void ) const {
		return m_regionLightmapOutputDirectory;
	}
	void setRegionLightmapProxyLod( int nLod );
	EA_FORCE_INLINE int getRegionLightmapProxyLod( void ) const {
		return m_nRegionLightmapProxyLod;
	}
	void setRegionLightmapContextRadius( int nRadius );
	EA_FORCE_INLINE int getRegionLightmapContextRadius( void ) const {
		return m_nRegionLightmapContextRadius;
	}
	void setRegionLightmapTexelSize( real_t fTexelSize );
	EA_FORCE_INLINE real_t getRegionLightmapTexelSize( void ) const {
		return m_fRegionLightmapTexelSize;
	}
	void setRegionLightmapBakeQuality( int nQuality );
	EA_FORCE_INLINE int getRegionLightmapBakeQuality( void ) const {
		return m_nRegionLightmapBakeQuality;
	}
	void setRegionLightmapShadowmaskMode( int nMode );
	EA_FORCE_INLINE int getRegionLightmapShadowmaskMode( void ) const {
		return m_nRegionLightmapShadowmaskMode;
	}
	void setHlodEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isHlodEnabled( void ) const {
		return m_bHlodEnabled;
	}
	void setHlodManifestPath( const String &rPath );
	EA_FORCE_INLINE String getHlodManifestPath( void ) const {
		return m_hlodManifestPath;
	}
	void setHlodTargetErrorPixels( real_t fPixels );
	EA_FORCE_INLINE real_t getHlodTargetErrorPixels( void ) const {
		return m_fHlodTargetErrorPixels;
	}
	void setHlodHysteresis( real_t fRatio );
	EA_FORCE_INLINE real_t getHlodHysteresis( void ) const {
		return m_fHlodHysteresis;
	}
	void setHlodMaxLevel( int nLevel );
	EA_FORCE_INLINE int getHlodMaxLevel( void ) const {
		return m_nHlodMaxLevel;
	}
	void setHlodLoadBudget( int nBudget );
	EA_FORCE_INLINE int getHlodLoadBudget( void ) const {
		return m_nHlodLoadBudget;
	}
	void setHlodUnloadBudget( int nBudget );
	EA_FORCE_INLINE int getHlodUnloadBudget( void ) const {
		return m_nHlodUnloadBudget;
	}
	void setHlodMemoryBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getHlodMemoryBudgetMb( void ) const {
		return m_nHlodMemoryBudgetMb;
	}
	void setHlodMaxShadowLevel( int nLevel );
	EA_FORCE_INLINE int getHlodMaxShadowLevel( void ) const {
		return m_nHlodMaxShadowLevel;
	}
	void setHlodForcedDebugLevel( int nLevel );
	EA_FORCE_INLINE int getHlodForcedDebugLevel( void ) const {
		return m_nHlodForcedDebugLevel;
	}
	void setEditorHlodPreview( bool bEnabled );
	EA_FORCE_INLINE bool isEditorHlodPreviewEnabled( void ) const {
		return m_bEditorHlodPreview;
	}
	EA_FORCE_INLINE bool isEditorHlodPreviewActive( void ) const {
		return isEditorAuthoringModeInternal() && m_bEditorHlodPreview;
	}
	bool reloadHlodManifest( void );
	Dictionary getHlodStreamingStats( void ) const;
	Array getHlodDebugEntries( void ) const;
	bool isHlodRegionSuppressed( const Vector2i &rRegion, int nMinimumLevel = 1 ) const;
	bool isHlodRegionDetailRequired( const Vector2i &rRegion ) const;
	bool isHlodRegionManaged( const Vector2i &rRegion ) const;
	uint8_t getHlodInstancerMinimumLevel( const Vector2i &rRegion, int nMeshId ) const;
	uint8_t getHlodNodeMinimumLevel( const Vector2i &rRegion, int64_t nSpawnId ) const;
	void onHlodRegionRepresentationChanged( const Vector2i &rRegion );
	bool isHlodDetailReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const;
	int64_t addStreamedScene( const Ref<PackedScene> &rScene,
			const Transform3D &rGlobalTransform,
			const Dictionary &rState = Dictionary(),
			int nFlags = 0,
			int nHlodMinLevel = 1 );
	int64_t addStreamedScenePath( const String &rScenePath,
			const Transform3D &rGlobalTransform,
			const Dictionary &rState = Dictionary(),
			int nFlags = 0,
			int nHlodMinLevel = 1 );
	bool removeStreamedScene( int64_t nSpawnId );
	int allocateStreamOwnerId( void );
	void setNextStreamOwnerId( int nOwnerId );
	EA_FORCE_INLINE int getNextStreamOwnerId( void ) const {
		return m_nNextStreamOwnerId;
	}
	Dictionary getNodeStreamingStats( void ) const;
	void setLabelDistance( const real_t fDistance );
	EA_FORCE_INLINE real_t getLabelDistance( void ) const {
		return m_fLabelDistance;
	}
	void setLabelSize( const int nSize );
	EA_FORCE_INLINE int getLabelSize( void ) const {
		return m_nLabelSize;
	}
	void updateRegionLabels( void );

	// Target Tracking
	void setCamera( Camera3D *pCamera );
	EA_FORCE_INLINE Camera3D *getCamera( void ) const {
		return cast_to<Camera3D>( m_hCamera.ptr() );
	}
	void setClipmapTarget( Node3D *pNode );
	EA_FORCE_INLINE Node3D *getClipmapTarget( void ) const {
		return m_clipmapTarget.ptr();
	}
	Vector3 getClipmapTargetPosition( void ) const;
	void setCollisionTarget( Node3D *pNode );
	EA_FORCE_INLINE Node3D *getCollisionTarget( void ) const {
		return m_collisionTarget.ptr();
	}
	Vector3 getCollisionTargetPosition( void ) const;
	void setLightTarget( Node3D *pNode );
	void setSemanticShadowPolicyEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isSemanticShadowPolicyEnabled( void ) const {
		return m_bSemanticShadowPolicyEnabled;
	}
	void setEnvironmentShadowQuality( t3dEnvironmentShadowQuality_t eQuality );
	EA_FORCE_INLINE t3dEnvironmentShadowQuality_t getEnvironmentShadowQuality( void ) const {
		return m_environmentShadowQuality;
	}
	void setEnvironmentShadowExternalMask( uint32_t nMask );
	EA_FORCE_INLINE uint32_t getEnvironmentShadowExternalMask( void ) const {
		return m_nEnvironmentShadowExternalMask;
	}
	void setSemanticDirectionalReceptionEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isSemanticDirectionalReceptionEnabled( void ) const {
		return m_bSemanticDirectionalReceptionEnabled;
	}
	void setEnvironmentDirectionalExternalMask( uint32_t nMask );
	EA_FORCE_INLINE uint32_t getEnvironmentDirectionalExternalMask( void ) const {
		return m_nEnvironmentDirectionalExternalMask;
	}
	EA_FORCE_INLINE Node3D *getLightTarget( void ) const {
		return m_lightTarget.ptr();
	}
	void snap( void );

	// Collision Aliases
	EA_FORCE_INLINE void setCollisionMode( const t3dCollisionMode_t mode ) {
		m_pCollision ? m_pCollision->setMode( mode ) : ( void )0;
	}
	EA_FORCE_INLINE t3dCollisionMode_t getCollisionMode( void ) const {
		return m_pCollision ? m_pCollision->getMode() : t3dCollisionMode_t::DYNAMIC_GAME;
	}
	EA_FORCE_INLINE void setCollisionShapeSize( const uint16_t nSize ) {
		m_pCollision ? m_pCollision->setShapeSize( nSize ) : ( void )0;
	}
	EA_FORCE_INLINE uint16_t getCollisionShapeSize( void ) const {
		return m_pCollision ? m_pCollision->getShapeSize() : 16;
	}
	EA_FORCE_INLINE void setCollisionRadius( const uint16_t nRadius ) {
		m_pCollision ? m_pCollision->setRadius( nRadius ) : ( void )0;
	}
	EA_FORCE_INLINE uint16_t getCollisionRadius( void ) const {
		return m_pCollision ? m_pCollision->getRadius() : 64;
	}
	EA_FORCE_INLINE void setCollisionLayer( const uint32_t nLayers ) {
		m_pCollision ? m_pCollision->setLayer( nLayers ) : ( void )0;
	}
	EA_FORCE_INLINE uint32_t getCollisionLayer( void ) const {
		return m_pCollision ? m_pCollision->getLayer() : 1;
	}
	EA_FORCE_INLINE void setCollisionMask( const uint32_t nMask ) {
		m_pCollision ? m_pCollision->setMask( nMask ) : ( void )0;
	}
	EA_FORCE_INLINE uint32_t getCollisionMask( void ) const {
		return m_pCollision ? m_pCollision->getMask() : 1;
	}
	EA_FORCE_INLINE void setCollisionPriority( const real_t fPriority ) {
		m_pCollision ? m_pCollision->setPriority( fPriority ) : ( void )0;
	}
	EA_FORCE_INLINE real_t getCollisionPriority( void ) const {
		return m_pCollision ? m_pCollision->getPriority() : 1.f;
	}
	EA_FORCE_INLINE void setPhysicsMaterial( const Ref<PhysicsMaterial> &rMat ) {
		m_pCollision ? m_pCollision->setPhysicsMaterial( rMat ) : ( void )0;
	}
	EA_FORCE_INLINE Ref<PhysicsMaterial> getPhysicsMaterial( void ) const {
		return m_pCollision ? m_pCollision->getPhysicsMaterial() : Ref<PhysicsMaterial>();
	}

	// Terrain Mesh
	EA_FORCE_INLINE Terrain3DMesher *getMesher( void ) const {
		return m_pTerrainMesher;
	}
	void setMaterial( const Ref<Terrain3DMaterial> &rMaterial );
	EA_FORCE_INLINE Ref<Terrain3DMaterial> getMaterial( void ) const {
		return m_material;
	}
	void setMeshLods( const int nCount );
	EA_FORCE_INLINE int getMeshLods( void ) const {
		return m_nMeshLods;
	}
	void setRuntimeShaderTieringEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isRuntimeShaderTieringEnabled( void ) const {
		return m_bRuntimeShaderTieringEnabled;
	}
	void setRuntimeNearShaderLods( int nCount );
	EA_FORCE_INLINE int getRuntimeNearShaderLods( void ) const {
		return m_nRuntimeNearShaderLods;
	}
	void setRuntimeMiddleShaderLods( int nCount );
	EA_FORCE_INLINE int getRuntimeMiddleShaderLods( void ) const {
		return m_nRuntimeMiddleShaderLods;
	}
	void setRuntimeNearRangeEndM( real_t fRangeM );
	real_t getRuntimeNearRangeEndM( void ) const;
	void setRuntimeMiddleRangeEndM( real_t fRangeM );
	real_t getRuntimeMiddleRangeEndM( void ) const;
	EA_FORCE_INLINE real_t getRuntimeShadowRangeEndM( void ) const {
		return lodCountToRange( m_nShadowLodCount );
	}
	void setRuntimeSyncDetailLods( bool bEnabled );
	EA_FORCE_INLINE bool isRuntimeSyncDetailLods( void ) const {
		return m_bRuntimeSyncDetailLods;
	}
	void setRuntimeClipmapUpdateBudget( int nCount );
	EA_FORCE_INLINE int getRuntimeClipmapUpdateBudget( void ) const {
		return m_nRuntimeClipmapUpdateBudget;
	}
	void setRuntimePagePrepareThreads( int nCount );
	EA_FORCE_INLINE int getRuntimePagePrepareThreads( void ) const {
		return m_nRuntimePagePrepareThreads;
	}
	void setTessellationLevel( const int nLevel );
	EA_FORCE_INLINE int getTessellationLevel( void ) const {
		return m_nTessellationLevel;
	}
	void setMeshSize( const int nSize );
	EA_FORCE_INLINE int getMeshSize( void ) const {
		return m_nMeshSize;
	}
	void setVertexSpacing( const real_t fSpacing );
	EA_FORCE_INLINE real_t getVertexSpacing( void ) const {
		return m_fVertexSpacing;
	}
	void setCullMargin( const real_t fMargin );
	EA_FORCE_INLINE real_t getCullMargin( void ) const {
		return m_fCullMargin;
	}
	void setCastShadows( const RenderingServer::ShadowCastingSetting castShadows );
	EA_FORCE_INLINE RenderingServer::ShadowCastingSetting getCastShadows( void ) const {
		return m_castShadows;
	}
	void setShadowLodCount( const int nCount );
	EA_FORCE_INLINE int getShadowLodCount( void ) const {
		return m_nShadowLodCount;
	}
	void setNearShadowEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getNearShadowEnabled( void ) const {
		return m_bNearShadowEnabled;
	}
	void setMiddleShadowEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getMiddleShadowEnabled( void ) const {
		return m_bMiddleShadowEnabled;
	}
	void setFarShadowEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getFarShadowEnabled( void ) const {
		return m_bFarShadowEnabled;
	}
	void setNearShadowLodCount( int nCount );
	EA_FORCE_INLINE int getNearShadowLodCount( void ) const {
		return m_nNearShadowLodCount;
	}
	void setMiddleShadowLodCount( int nCount );
	EA_FORCE_INLINE int getMiddleShadowLodCount( void ) const {
		return m_nMiddleShadowLodCount;
	}
	void setFarShadowLodCount( int nCount );
	EA_FORCE_INLINE int getFarShadowLodCount( void ) const {
		return m_nFarShadowLodCount;
	}
	void setGiMode( const GeometryInstance3D::GIMode giMode );
	EA_FORCE_INLINE GeometryInstance3D::GIMode getGiMode( void ) const {
		return m_giMode;
	}
	void setRenderLayers( const uint32_t nLayers );
	EA_FORCE_INLINE uint32_t getRenderLayers( void ) const {
		return m_nRenderLayers;
	}

	void setRenderBackend( t3dRenderBackend_t backend );
	EA_FORCE_INLINE t3dRenderBackend_t getRenderBackend( void ) const { return m_renderBackend; }
	void setRdNativeProxyMode( t3dRdNativeProxyMode_t mode );
	EA_FORCE_INLINE t3dRdNativeProxyMode_t getRdNativeProxyMode( void ) const { return m_rdNativeProxyMode; }
	Dictionary getRdRendererDiagnostics( void ) const;

	// Material Displacement Aliases
	EA_FORCE_INLINE void setDisplacementScale( const real_t fDisplacementScale ) {
		m_material.is_valid() ? m_material->setDisplacementScale( fDisplacementScale ) : ( void )0;
	}
	EA_FORCE_INLINE real_t getDisplacementScale( void ) const {
		return m_material.is_valid() ? m_material->getDisplacementScale() : 1.f;
	}
	EA_FORCE_INLINE void setDisplacementSharpness( const real_t fDisplacementSharpness ) {
		m_material.is_valid() ? m_material->setDisplacementSharpness( fDisplacementSharpness ) : ( void )0;
	}
	EA_FORCE_INLINE real_t getDisplacementSharpness( void ) const {
		return m_material.is_valid() ? m_material->getDisplacementSharpness() : 0.25f;
	}
	EA_FORCE_INLINE void setBufferShaderOverrideEnabled( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setBufferShaderOverrideEnabled( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool isBufferShaderOverrideEnabled( void ) const {
		return m_material.is_valid() ? m_material->isBufferShaderOverrideEnabled() : false;
	}
	EA_FORCE_INLINE void setBufferShaderOverride( const Ref<Shader> &rShader ) {
		return m_material.is_valid() ? m_material->setBufferShaderOverride( rShader ) : ( void )0;
	}
	EA_FORCE_INLINE Ref<Shader> getBufferShaderOverride( void ) const {
		return m_material.is_valid() ? m_material->getBufferShaderOverride() : Ref<Shader>();
	}

	// Ocean Mesh
	EA_FORCE_INLINE Terrain3DMesher *getOceanMesher( void ) const {
		return m_pOceanMesher;
	}
	void setOceanEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool isOceanEnabled( void ) const {
		return m_bOceanEnabled;
	}
	void setOceanMeshLods( const int nCount );
	EA_FORCE_INLINE int getOceanMeshLods( void ) const {
		return m_nOceanMeshLods;
	}
	void setOceanTessellationLevel( const int nLevel );
	EA_FORCE_INLINE int getOceanTessellationLevel( void ) const {
		return m_nOceanTessellationLevel;
	}
	void setOceanMeshSize( const int nSize );
	EA_FORCE_INLINE int getOceanMeshSize( void ) const {
		return m_nOceanMeshSize;
	}
	void setOceanVertexSpacing( const real_t fSpacing );
	EA_FORCE_INLINE real_t getOceanVertexSpacing( void ) const {
		return m_fOceanVertexSpacing;
	}
	void setOceanCullMargin( const real_t fMargin );
	EA_FORCE_INLINE real_t getOceanCullMargin( void ) const {
		return m_fOceanCullMargin;
	}
	void setOceanCastShadows( const RenderingServer::ShadowCastingSetting castShadows );
	EA_FORCE_INLINE RenderingServer::ShadowCastingSetting getOceanCastShadows( void ) const {
		return m_oceanCastShadows;
	}
	void setOceanGiMode( const GeometryInstance3D::GIMode giMode );
	EA_FORCE_INLINE GeometryInstance3D::GIMode getOceanGiMode( void ) const {
		return m_oceanGiMode;
	}
	void setOceanRenderLayers( const uint32_t nLayers );
	EA_FORCE_INLINE uint32_t getOceanRenderLayers( void ) const {
		return m_nOceanRenderLayers;
	}
	void setOceanPerformancePreset( const t3dOceanPerformancePreset_t preset );
	EA_FORCE_INLINE t3dOceanPerformancePreset_t getOceanPerformancePreset( void ) const {
		return m_oceanPerformancePreset;
	}
	void setOceanVertexWaveIterations( const int nIterations );
	EA_FORCE_INLINE int getOceanVertexWaveIterations( void ) const {
		return m_nOceanVertexWaveIterations;
	}
	void setOceanFragmentWaveIterations( const int nIterations );
	EA_FORCE_INLINE int getOceanFragmentWaveIterations( void ) const {
		return m_nOceanFragmentWaveIterations;
	}
	void setOceanRefractionMode( const t3dOceanRefractionMode_t mode );
	EA_FORCE_INLINE t3dOceanRefractionMode_t getOceanRefractionMode( void ) const {
		return m_oceanRefractionMode;
	}
	void setOceanFoamMode( const t3dOceanFoamMode_t mode );
	EA_FORCE_INLINE t3dOceanFoamMode_t getOceanFoamMode( void ) const {
		return m_oceanFoamMode;
	}
	void setOceanScatteringMode( const t3dOceanScatteringMode_t mode );
	EA_FORCE_INLINE t3dOceanScatteringMode_t getOceanScatteringMode( void ) const {
		return m_oceanScatteringMode;
	}
	void setOceanShadingMode( const t3dOceanShadingMode_t mode );
	EA_FORCE_INLINE t3dOceanShadingMode_t getOceanShadingMode( void ) const {
		return m_oceanShadingMode;
	}
	void setOceanMaterial( const Ref<Material> &rMaterial );
	EA_FORCE_INLINE Ref<Material> getOceanMaterial( void ) const {
		return m_oceanMaterial;
	}

	// Rendering
	void setMouseLayer( const uint32_t nLayer );
	EA_FORCE_INLINE uint32_t getMouseLayer( void ) const {
		return m_nMouseLayer;
	}
	EA_FORCE_INLINE void setFreeEditorTextures( const bool bFreeTextures ) {
		m_bFreeEditorTextures = bFreeTextures;
	}
	EA_FORCE_INLINE bool getFreeEditorTextures( void ) const {
		return m_bFreeEditorTextures;
	}
	EA_FORCE_INLINE void setInstancerMode( const t3dInstancerMode_t mode ) {
		m_pInstancer ? m_pInstancer->setMode( mode ) : ( void )0;
	}
	EA_FORCE_INLINE t3dInstancerMode_t getInstancerMode( void ) const {
		return m_pInstancer ? m_pInstancer->getMode() : t3dInstancerMode_t::NORMAL;
	}

	// Utility
	Vector3 getIntersection( const Vector3 &rSrcPos, const Vector3 &rDirection, const bool bGpuMode = false );
	Dictionary getRaycastResult( const Vector3 &rSrcPos,
			const Vector3 &rDirection,
			const uint32_t nColMask = 0xFFFFFFFF,
			const bool bExcludeSelf = false ) const;
	Ref<Mesh> bakeMesh( const int nLod, const t3dHeightFilter_t filter = HEIGHT_FILTER_NEAREST ) const;
	Ref<Mesh> bakeRegionLightmapMesh( const Vector2i &rRegionLocation,
			int nLod = 2,
			int nContextRadiusRegions = 1,
			real_t fTexelSize = 1.0f ) const;
	Ref<Mesh> bakeLightmapClusterMesh( const Vector2i &rRegionMin,
			const Vector2i &rRegionMax,
			int nLod = 2,
			int nContextRadiusRegions = 1,
			real_t fTexelSize = 1.0f ) const;
	Ref<ArrayOccluder3D> bakeOccluder( const int nLod ) const;
	PackedVector3Array generateNavMeshSourceGeometry( const AABB &rGlobalAabb, const bool bRequireNav = true ) const;

	// Warnings
	void setWarning( const uint8_t nWarning, const bool bEnabled );
	EA_FORCE_INLINE uint8_t getWarnings( void ) const {
		return m_nWarnings;
	}
	PackedStringArray _get_configuration_warnings( void ) const override;

	// Overlay Aliases
	EA_FORCE_INLINE void setShowRegionGrid( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowRegionGrid( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowRegionGrid( void ) const {
		return m_material.is_valid() ? m_material->getShowRegionGrid() : false;
	}
	EA_FORCE_INLINE void setShowInstancerGrid( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowInstancerGrid( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowInstancerGrid( void ) const {
		return m_material.is_valid() ? m_material->getShowInstancerGrid() : false;
	}
	EA_FORCE_INLINE void setShowVertexGrid( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowVertexGrid( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowVertexGrid( void ) const {
		return m_material.is_valid() ? m_material->getShowVertexGrid() : false;
	}
	EA_FORCE_INLINE void setShowContours( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowContours( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowContours( void ) const {
		return m_material.is_valid() ? m_material->getShowContours() : false;
	}
	EA_FORCE_INLINE void setShowNavigation( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowNavigation( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowNavigation( void ) const {
		return m_material.is_valid() ? m_material->getShowNavigation() : false;
	}

	void setComputeDisplacementEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getComputeDisplacementEnabled( void ) const {
		return m_bComputeDisplacementEnabled.load( std::memory_order_acquire );
	}
	void setDisplacementUpdateDistance( const real_t fDistance );
	EA_FORCE_INLINE real_t getDisplacementUpdateDistance( void ) const {
		return m_fDisplacementUpdateDistance;
	}
	EA_FORCE_INLINE bool isComputeDisplacementActive( void ) const {
		return m_bComputeDisplacementActive.load( std::memory_order_acquire );
	}
	RID getComputeDisplacementTextureRid( void ) const;

	// Debug View Aliases
	EA_FORCE_INLINE void setShowCheckered( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowCheckered( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowCheckered( void ) const {
		return m_material.is_valid() ? m_material->getShowCheckered() : false;
	}
	EA_FORCE_INLINE void setShowGrey( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowGrey( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowGrey( void ) const {
		return m_material.is_valid() ? m_material->getShowGrey() : false;
	}
	EA_FORCE_INLINE void setShowHeightmap( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowHeightmap( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowHeightmap( void ) const {
		return m_material.is_valid() ? m_material->getShowHeightmap() : false;
	}
	EA_FORCE_INLINE void setShowJaggedness( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowJaggedness( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowJaggedness( void ) const {
		return m_material.is_valid() ? m_material->getShowJaggedness() : false;
	}
	EA_FORCE_INLINE void setShowAutoshader( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowAutoshader( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowAutoshader( void ) const {
		return m_material.is_valid() ? m_material->getShowAutoshader() : false;
	}
	EA_FORCE_INLINE void setShowControlTexture( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowControlTexture( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowControlTexture( void ) const {
		return m_material.is_valid() ? m_material->getShowControlTexture() : false;
	}
	EA_FORCE_INLINE void setShowControlBlend( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowControlBlend( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowControlBlend( void ) const {
		return m_material.is_valid() ? m_material->getShowControlBlend() : false;
	}
	EA_FORCE_INLINE void setShowControlAngle( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowControlAngle( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowControlAngle( void ) const {
		return m_material.is_valid() ? m_material->getShowControlAngle() : false;
	}
	EA_FORCE_INLINE void setShowControlScale( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowControlScale( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowControlScale( void ) const {
		return m_material.is_valid() ? m_material->getShowControlScale() : false;
	}
	EA_FORCE_INLINE void setShowColormap( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowColormap( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowColormap( void ) const {
		return m_material.is_valid() ? m_material->getShowColormap() : false;
	}
	EA_FORCE_INLINE void setShowRoughmap( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowRoughmap( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowRoughmap( void ) const {
		return m_material.is_valid() ? m_material->getShowRoughmap() : false;
	}
	EA_FORCE_INLINE void setShowDisplacementBuffer( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowDisplacementBuffer( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowDisplacementBuffer( void ) const {
		return m_material.is_valid() ? m_material->getShowDisplacementBuffer() : false;
	}

	// PBR View Aliases
	EA_FORCE_INLINE void setShowTextureAlbedo( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowTextureAlbedo( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowTextureAlbedo( void ) const {
		return m_material.is_valid() ? m_material->getShowTextureAlbedo() : false;
	}
	EA_FORCE_INLINE void setShowTextureHeight( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowTextureHeight( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowTextureHeight( void ) const {
		return m_material.is_valid() ? m_material->getShowTextureHeight() : false;
	}
	EA_FORCE_INLINE void setShowTextureNormal( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowTextureNormal( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowTextureNormal( void ) const {
		return m_material.is_valid() ? m_material->getShowTextureNormal() : false;
	}
	EA_FORCE_INLINE void setShowTextureRough( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowTextureRough( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowTextureRough( void ) const {
		return m_material.is_valid() ? m_material->getShowTextureRough() : false;
	}
	EA_FORCE_INLINE void setShowTextureAo( const bool bEnabled ) {
		m_material.is_valid() ? m_material->setShowTextureAo( bEnabled ) : ( void )0;
	}
	EA_FORCE_INLINE bool getShowTextureAo( void ) const {
		return m_material.is_valid() ? m_material->getShowTextureAo() : false;
	}

protected:
	void _notification( const int nWhat );
	void _validate_property( PropertyInfo &rProperty ) const;
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dPerformanceTarget_t );
VARIANT_ENUM_CAST( t3dPerformanceProfileStatus_t );
VARIANT_ENUM_CAST( t3dEditorExecutionMode_t );
VARIANT_ENUM_CAST( t3dRegionSize_t );
VARIANT_ENUM_CAST( t3dDebugLevel_t );

VARIANT_ENUM_CAST( t3dOceanPerformancePreset_t );
VARIANT_ENUM_CAST( t3dOceanRefractionMode_t );
VARIANT_ENUM_CAST( t3dOceanFoamMode_t );
VARIANT_ENUM_CAST( t3dOceanScatteringMode_t );
VARIANT_ENUM_CAST( t3dOceanShadingMode_t );

VARIANT_ENUM_CAST( t3dEnvironmentShadowQuality_t );
#endif // TERRAIN3D_CLASS_H
