// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_CLASS_H
#define TERRAIN3D_CLASS_H

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

#include "eastl_config.h"
#include <EASTL/array.h>
#include <atomic>
#include <mutex>
#include <EASTL/vector.h>

#include "Terrain3DConstants.h"
#include "core/target_node_3d.h"
#include "streaming/terrain_3d_adaptive_budget_policy.h"
#include "streaming/terrain_3d_boundary_phase_policy.h"
#include "assets/terrain_3d_assets.h"
#include "physics/terrain_3d_collision.h"
#include "rendering/terrain_3d_compute.h"
#include "data/terrain_3d_data.h"
#include "core/terrain_3d_diagnostics.h"
#include "instances/terrain_3d_instancer.h"
#include "rendering/terrain_3d_material.h"
#include "memory/terrain_3d_memory_budget.h"
#include "memory/terrain_3d_memory_arena.h"
#include "memory/terrain_3d_memory_zone.h"
#include "rendering/terrain_3d_mesher.h"
#include "streaming/terrain_3d_streaming_policy.h"

class T3DEditor;
class T3DNodeStreamer;
class T3DLightmapStreamer;
class T3DHLODStreamer;

class T3DTerrain : public Node3D {
	using Terrain3D = T3DTerrain;
	GDCLASS( Terrain3D, Node3D );
	CLASS_NAME();

public: // Constants
	typedef enum t3ddebugLevel_e {
		MESG = -2,	 // Always print except in release builds
		WARN = -1,	 // Always print except in release builds
		ERROR = 0,	 // Always print except in release builds
		INFO = 1,	 // Print every function call and important entries
		DEBUG = 2,	 // Print details within functions
		EXTREME = 3, // Continuous operations like snapping
	} t3ddebugLevel_t;

	typedef enum t3dperformanceTarget_e {
		PERFORMANCE_TARGET_CUSTOM,
		PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL,
	} t3dperformanceTarget_t;

	typedef enum t3dperformanceProfileStatus_e {
		PERFORMANCE_PROFILE_CUSTOM,
		PERFORMANCE_PROFILE_EXACT,
		PERFORMANCE_PROFILE_MODIFIED,
	} t3dperformanceProfileStatus_t;

	typedef enum t3deditorExecutionMode_e {
		EDITOR_EXECUTION_AUTHORING = TERRAIN3D_EDITOR_EXECUTION_AUTHORING,
		EDITOR_EXECUTION_RUNTIME_PROFILE = TERRAIN3D_EDITOR_EXECUTION_RUNTIME_PROFILE,
	} t3deditorExecutionMode_t;


	typedef enum t3denvironmentShadowQuality_e {
		ENVIRONMENT_SHADOW_LOW,
		ENVIRONMENT_SHADOW_NORMAL,
		ENVIRONMENT_SHADOW_HIGH,
		ENVIRONMENT_SHADOW_ULTRA,
	} t3denvironmentShadowQuality_t;

	typedef enum t3dregionSize_e {
		SIZE_64 = 64,
		SIZE_128 = 128,
		SIZE_256 = 256,
		SIZE_512 = 512,
		SIZE_1024 = 1024,
		SIZE_2048 = 2048,
	} t3dregionSize_t;

	typedef enum t3doceanPerformancePreset_e {
		OCEAN_PRESET_CUSTOM,
		OCEAN_PRESET_BORDER_MINIMUM,
		OCEAN_PRESET_BORDER_BALANCED,
		OCEAN_PRESET_HIGH,
		OCEAN_PRESET_CINEMATIC,
	} t3doceanPerformancePreset_t;
	typedef enum t3doceanRefractionMode_e {
		OCEAN_REFRACTION_OFF,
		OCEAN_REFRACTION_DEPTH_TINT,
		OCEAN_REFRACTION_FULL
	} t3doceanRefractionMode_t;
	typedef enum t3doceanFoamMode_e {
		OCEAN_FOAM_OFF,
		OCEAN_FOAM_SHORE,
		OCEAN_FOAM_FULL
	} t3doceanFoamMode_t;
	typedef enum t3doceanScatteringMode_e {
		OCEAN_SCATTERING_OFF,
		OCEAN_SCATTERING_APPROXIMATE,
		OCEAN_SCATTERING_FULL
	} t3doceanScatteringMode_t;
	typedef enum t3doceanShadingMode_e {
		OCEAN_SHADING_LAMBERT_DIFFUSE,
		OCEAN_SHADING_LAMBERT_GGX,
		OCEAN_SHADING_BURLEY_GGX,
		OCEAN_SHADING_UNSHADED,
		OCEAN_SHADING_LAMBERT_VERTEX
	} t3doceanShadingMode_t;

private:
	String m_version = "1.1.0-dev";
	String m_dataDirectory;
	bool m_bIsInsideWorld = false;
	bool m_bInitialized = false;
	uint8_t m_nWarnings = 0u;
	// M1/M2 ownership root. Declare these before containers so reverse member
	// destruction drains all zone-backed allocations before the arena is released.
	T3DMemoryArena m_runtimeMemoryArena;
	T3DMemoryZone m_runtimeMemoryZone;
	t3dmemoryArenaSlice_t m_runtimeActiveHeightSlice;
	t3dmemoryArenaSlice_t m_runtimeZoneSlice;
	bool m_bRuntimeMemoryReady = false;

	// Object references
	T3DData *m_pData = nullptr;
	Ref<T3DAssets> m_pAssets;
	Ref<Resource> m_pAutoShaderProfile;
	T3DCollision *m_pCollision = nullptr;
	T3DInstancer *m_pInstancer = nullptr;
	T3DNodeStreamer *m_pNodeStreamer = nullptr;
	T3DLightmapStreamer *m_pLightmapStreamer = nullptr;
	T3DHLODStreamer *m_pHlodStreamer = nullptr;
	T3DEditor *m_pEditor = nullptr;
	Object *m_pEditorPlugin = nullptr;

	// Regions
	t3dregionSize_t m_nRegionSize = SIZE_128;
	bool m_bSave16Bit = false;
	t3dperformanceTarget_t m_performanceTarget = PERFORMANCE_TARGET_CUSTOM;
	t3dperformanceProfileStatus_t m_performanceProfileStatus = PERFORMANCE_PROFILE_CUSTOM;
	bool m_bApplyingPerformanceTarget = false;
	bool m_bPerformanceProfileMaterialPendingApply = false;
	// Runtime settings remain stored in every mode. Authoring changes only the
	// effective editor residency policy; profiling uses the exact runtime path.
	t3deditorExecutionMode_t m_editorExecutionMode = EDITOR_EXECUTION_AUTHORING;
	// Editor-only node preview keeps terrain authoring residency intact while
	// exercising the shipping dynamic-node streamer around the editor camera.
	bool m_bEditorNodeStreamingPreview = false;
	// HLOD preview uses shipping representation selection while terrain stays in Authoring mode.
	bool m_bEditorHlodPreview = false;

	// Unified logical region-streaming policy. Every subsystem consumes this map;
	// no renderer/object cache is allowed to invent a separate world footprint.
	t3dregionStreamingConfig_t m_regionStreamingConfig{};
	uint8_t m_szRegionStreamingStates[TERRAIN3D_MAX_REGION_SLOTS] = {};
	uint8_t m_szRegionStreamingPrefetch[TERRAIN3D_MAX_REGION_SLOTS] = {};
	Vector2i m_regionStreamingCenter = V2I_MAX;
	Vector2i m_regionStreamingPredictedCenter = V2I_MAX;
	Vector3 m_regionStreamingPreviousWorldPosition = V3_MAX;
	double m_fRegionStreamingVelocityX = 0.0;
	double m_fRegionStreamingVelocityZ = 0.0;
	int m_nRegionStreamingActiveCount = 0;
	int m_nRegionStreamingHotCount = 0;
	int m_nRegionStreamingColdCount = 0;
	int m_nRegionStreamingUnloadedCount = 0;
	int m_nRegionStreamingPrefetchCount = 0;
	uint64_t m_nRegionStreamingGeneration = 0u;
	bool m_bRegionStreamingDirty = true;
	t3druntimeProfileAccumulator_t m_runtimeProfileAccumulator;
	t3druntimeProfileHistory_t *m_pRuntimeProfileHistory = nullptr;
	t3druntimeProfileSample_t m_runtimeProfileLastSample;
	uint64_t m_nRuntimeProfileSampleCount = 0u;
	uint64_t m_nRuntimeProfileShaderUpdatesSeen = 0u;
	uint64_t m_runtimeProfileShaderCreationsSeen = 0u;
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
	t3dadaptiveBudgetState_t m_nRuntimeAdaptiveBudgetState;
	bool m_bRuntimeBoundaryWorkPhasingEnabled = true;
	int m_nRuntimeBoundaryNodeDelayFrames = 1;
	int m_nRuntimeBoundaryInstancerDelayFrames = 2;
	uint64_t m_iRuntimePhysicsFrameIndex = 0u;
	uint64_t m_nRuntimeBoundaryTransitionFrame = 0u;
	uint64_t m_nRuntimeBoundaryNodeDeferralsTotal = 0u;
	uint64_t m_nRuntimeBoundaryInstancerDeferralsTotal = 0u;
	int m_runtimeBoundaryNodeDeferralStreak = 0;
	int m_runtimeBoundaryInstancerDeferralStreak = 0;
	Vector2i m_runtimeBoundaryTargetRegion = V2I_MAX;
	bool m_bRuntimeBoundaryNodeDeferredLastTick = false;
	bool m_bRuntimeBoundaryInstancerDeferredLastTick = false;
	bool m_bGpuRegionCacheEnabled = false;
	bool m_bDeferGpuRegionCacheReconfigure = false;
	int m_nGpuRegionCacheBudgetMb = 16;
	int m_nGpuRegionUploadBudgetLayers = 4;
	int m_nGpuRegionUploadBudgetMb = 2;
	int m_nGpuRegionSparePages = TERRAIN3D_GPU_CACHE_DEFAULT_SPARE_LAYERS;
	int m_nGpuRegionRetireFrames = TERRAIN3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
	int m_nGpuRegionDetailLods = 0;
	int m_nGpuRegionGuardRegions = TERRAIN3D_GPU_CACHE_DEFAULT_GUARD_REGIONS;
	int m_nGpuRegionPrefetchRows = TERRAIN3D_GPU_CACHE_DEFAULT_PREFETCH_ROWS;
	int m_nGpuRegionHysteresisTicks = TERRAIN3D_GPU_CACHE_DEFAULT_HYSTERESIS_TICKS;
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
	int m_nInstancerCellSize = TERRAIN3D_INSTANCE_DEFAULT_CELL_SIZE;
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

	// PATCH-07 native total-memory coordinator. Requested subsystem budgets are
	// retained as manual override values; effective budgets are derived at the
	// low-frequency policy cadence while auto mode is enabled.
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
	t3dmemoryBudgetResult_t m_nMemoryBudgetResult;
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
	T3DTargetNode3D m_clipmapTarget;
	T3DTargetNode3D m_collisionTarget;
	T3DTargetNode3D m_lightTarget;
	T3DTargetNode3D m_camera; // Fallback target for clipmap and collision
	Color m_lastLightColor = COLOR_WHITE;
	Vector3 m_lastLightDirection = V3_ZERO;
	bool m_bLightStateValid = false;
	bool m_bSemanticShadowPolicyEnabled = false;
	t3denvironmentShadowQuality_t m_environmentShadowQuality = ENVIRONMENT_SHADOW_NORMAL;
	uint32_t m_nEnvironmentShadowExternalMask = 1u;
	uint32_t m_nLightShadowCasterMaskBeforeSemantic = 0xFFFFFu;
	bool m_bLightShadowMaskCaptured = false;
	bool m_bSemanticDirectionalReceptionEnabled = false;
	uint32_t m_nEnvironmentDirectionalExternalMask = 0u;
	uint32_t m_nLightCullMaskBeforeSemantic = 0xFFFFFu;
	bool m_bLightCullMaskCaptured = false;

	// Terrain Mesh
	T3DMesher *m_pTerrainMesher = nullptr;
	Ref<T3DMaterial> m_pMaterial;
	int m_nMeshLods = 7;
	int m_tessellationLevel = 0;
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
	uint32_t m_renderLayers = 1u | ( 1u << 31u ); // Bit 1 and 32 for the cursor

	// Displacement Buffer
	SubViewport *m_pDBufferVp = nullptr;
	ColorRect *m_pDBufferRect = nullptr;
	Vector2 m_lastBufferPosition = V2_MAX;
	std::atomic_bool m_bComputeDisplacementEnabled{false};
	std::atomic_bool m_bComputeDisplacementActive{false};
	std::atomic_bool m_bComputeDisplacementDispatchPending{false};
	std::atomic_bool m_bComputeDisplacementFailed{false};
	std::atomic_bool m_bComputeDisplacementFallbackRequested{false};
	std::atomic_bool m_bComputeDisplacementTearingDown{false};
	real_t m_fDisplacementUpdateDistance = 0.25f;
	std::mutex m_computeDisplacementMutex;
	T3DCompute::t3ddisplacementRequest_t m_computeDisplacementRequest;
	uint64_t m_computeDisplacementRequestVersion = 0u;

	// Ocean Mesh
	T3DMesher *m_pOceanMesher = nullptr;
	bool m_bOceanEnabled = false;
	int m_nOceanMeshLods = 7;
	int m_oceanTessellationLevel = 0;
	int m_nOceanMeshSize = 32;
	real_t m_fOceanVertexSpacing = 4.0f;
	real_t m_fOceanCullMargin = 20.0f;
	RenderingServer::ShadowCastingSetting m_oceanCastShadows = RenderingServer::SHADOW_CASTING_SETTING_OFF;
	GeometryInstance3D::GIMode m_oceanGiMode = GeometryInstance3D::GI_MODE_DISABLED;
	uint32_t m_oceanRenderLayers = 1u;
	Ref<Material> m_pOceanMaterial;
	Ref<Shader> m_pOceanVariantShader;
	String m_oceanVariantSource;
	t3doceanPerformancePreset_t m_oceanPerformancePreset = OCEAN_PRESET_BORDER_BALANCED;
	int m_nOceanVertexWaveIterations = 1;
	int m_nOceanFragmentWaveIterations = 1;
	t3doceanRefractionMode_t m_oceanRefractionMode = OCEAN_REFRACTION_DEPTH_TINT;
	t3doceanFoamMode_t m_oceanFoamMode = OCEAN_FOAM_SHORE;
	t3doceanScatteringMode_t m_oceanScatteringMode = OCEAN_SCATTERING_APPROXIMATE;
	t3doceanShadingMode_t m_oceanShadingMode = OCEAN_SHADING_LAMBERT_GGX;

	// Rendering
	bool m_bFreeEditorTextures = true;
	// Mouse cursor
	SubViewport *m_pMouseVp = nullptr;
	Camera3D *m_pMouseCam = nullptr;
	MeshInstance3D *m_pMouseQuad = nullptr;
	uint32_t m_mouseLayer = 32u;
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
	t3dmemoryBudgetInput_t buildMemoryBudgetInput( void ) const;
	uint64_t calculateFixedMemoryBytes( void ) const;
	uint64_t calculateTransientMemoryBytes( void ) const;
	void applyMemoryBudgetResult( const t3dmemoryBudgetResult_t &rResult, bool bForce );
	static const char *memoryLimitReasonName( t3dmemoryLimitReason_t reason );
	void sampleRuntimeProfile( const t3druntimeProfileSample_t &rSample );
	void updateCacheThrashEvents( const t3druntimeProfileSample_t &rSample );
	Dictionary getRuntimeMemoryStatsFast( void ) const;
	Array getShaderTierStatsDictionaries( void ) const;
	Array getCacheInventory( bool bDeep ) const;
	Dictionary getCpuProfileStats( void ) const;
	Dictionary getGpuInformationStats( void ) const;
	Dictionary runtimeProfileSampleToDictionary( const t3druntimeProfileSample_t &rSample ) const;
	Array getRuntimeProfileEventNames( uint32_t nFlags ) const;
	Array getRuntimeProfileStallNames( uint32_t nFlags ) const;
	Array getRuntimeProfileProblems( void ) const;
	Dictionary getRuntimeProfileAggregate( void ) const;
	t3dadaptiveBudgetLimits_t getRuntimeAdaptiveBudgetLimits( void ) const;
	t3dadaptiveBudgetPressure_t getRuntimeAdaptiveBudgetPressure( void ) const;
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
	void connectResourceSignals( const Ref<T3DAssets> &rAssets, const Ref<T3DMaterial> &rMaterial );
	void disconnectResourceSignals( const Ref<T3DAssets> &rAssets, const Ref<T3DMaterial> &rMaterial );
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
	void updateMesherAabbs( void ) {
		m_pTerrainMesher ? m_pTerrainMesher->updateAabbs() : ( void ) 0;
	}
	void destroyTerrainMesher( const bool bFinal = false );
	void setupOceanMesher( void );
	void updateOceanShaderVariant( void );
	void updateOceanAabbs( void ) {
		m_pOceanMesher ? m_pOceanMesher->updateAabbs() : ( void ) 0;
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

	typedef struct t3dtriangleBuffer_s {
		eastl::vector<Vector3> ownedVertices;
		eastl::vector<Vector2> ownedUvs;
		Vector3 *pExternalVertices = nullptr;
		Vector2 *pExternalUvs = nullptr;
		size_t nExternalCapacity = 0u;
		size_t nExternalSize = 0u;
		bool bOverflowed = false;

		void bindExternal( Vector3 *const pVertices, Vector2 *const pUvs, const size_t nCapacity ) {
			pExternalVertices = pVertices;
			pExternalUvs = pUvs;
			nExternalCapacity = nCapacity;
			nExternalSize = 0u;
			bOverflowed = false;
		}

		void reserve( const size_t nCapacity, const bool bUvs ) {
			ownedVertices.reserve( nCapacity );
			if ( bUvs ) {
				ownedUvs.reserve( nCapacity );
			}
		}

		void appendTriangle( const Vector3 &rA, const Vector3 &rB, const Vector3 &rC, const bool bUvs ) {
			if ( pExternalVertices ) {
				if ( nExternalSize + 3u > nExternalCapacity ) {
					bOverflowed = true;
					return;
				}
				pExternalVertices[nExternalSize] = rA;
				pExternalVertices[nExternalSize + 1u] = rB;
				pExternalVertices[nExternalSize + 2u] = rC;
				if ( bUvs && pExternalUvs ) {
					pExternalUvs[nExternalSize] = Vector2( rA.x, rA.z );
					pExternalUvs[nExternalSize + 1u] = Vector2( rB.x, rB.z );
					pExternalUvs[nExternalSize + 2u] = Vector2( rC.x, rC.z );
				}
				nExternalSize += 3u;
				return;
			}

			ownedVertices.push_back( rA );
			ownedVertices.push_back( rB );
			ownedVertices.push_back( rC );
			if ( bUvs ) {
				ownedUvs.push_back( Vector2( rA.x, rA.z ) );
				ownedUvs.push_back( Vector2( rB.x, rB.z ) );
				ownedUvs.push_back( Vector2( rC.x, rC.z ) );
			}
		}

		size_t size( void ) const {
			return pExternalVertices ? nExternalSize : ownedVertices.size();
		}

		size_t uvSize( void ) const {
			return pExternalVertices ? ( pExternalUvs ? nExternalSize : 0u ) : ownedUvs.size();
		}

		const Vector3 *vertexData( void ) const {
			return pExternalVertices ? pExternalVertices : ownedVertices.data();
		}

		const Vector2 *uvData( void ) const {
			return pExternalVertices ? pExternalUvs : ownedUvs.data();
		}
	} t3dtriangleBuffer_t;

	typedef struct t3dbakeRowTask_s {
		int32_t xStart = 0;
		int32_t xEnd = 0;
		int32_t zStart = 0;
		int32_t zEnd = 0;
		size_t iOutputOffset = 0u;
		size_t nOutputCapacity = 0u;
	} t3dbakeRowTask_t;

	typedef struct t3dbakeRegionSnapshot_s {
		Vector2i location;
		PackedByteArray heightData;
		PackedByteArray controlData;
		const uint8_t *pHeightBytes = nullptr;
		const uint8_t *pControlBytes = nullptr;
	} t3dbakeRegionSnapshot_t;

	typedef struct t3dbakeSnapshot_s {
		int32_t nRegionSize = 0;
		real_t fVertexSpacing = 1.f;
		int32_t szRegionLookup[TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE * TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE] = {};
		eastl::vector<t3dbakeRegionSnapshot_t> regions;

		const t3dbakeRegionSnapshot_t *getRegion( const int32_t iX, const int32_t iZ ) const;
		void getSample( const int32_t iX, const int32_t iZ, uint32_t &nControl, real_t &fHeight ) const;
		bool getMinimumRect( int32_t iX, int32_t iZ, int32_t nWidth, int32_t nHeight, real_t &fHeight ) const;
		Vector3 getMeshVertex(
			const int32_t nLod, const T3DData::t3dheightFilter_t filter, const int32_t iX,
			const int32_t iZ, uint32_t &nControl
		) const;
	} t3dbakeSnapshot_t;

	typedef struct t3dbakeTaskContext_s {
		const T3DTerrain *pTerrain = nullptr;
		const t3dbakeSnapshot_t *pSnapshot = nullptr;
		const eastl::vector<t3dbakeRowTask_t> *pTasks = nullptr;
		eastl::vector<t3dtriangleBuffer_t> *pBuffers = nullptr;
		int32_t step = 1;
		int32_t lod = 0;
		T3DData::t3dheightFilter_t filter = T3DData::HEIGHT_FILTER_NEAREST;
		bool bGenerateUvs = false;
		bool bRequireNav = false;
	} t3dbakeTaskContext_t;

	static bool bakeRegionSnapshotLess( const t3dbakeRegionSnapshot_t &rA, const t3dbakeRegionSnapshot_t &rB );
	static void runBakeTaskRange( void *pContext, size_t nBegin, size_t nEnd );
	Vector3 getTriangleVertex(
		const t3dbakeSnapshot_t *pSnapshot, int32_t nLod, T3DData::t3dheightFilter_t filter,
		int32_t iX, int32_t iZ, const Vector3 &rPosition, uint32_t &nControl
	) const;

	t3dbakeSnapshot_t createBakeSnapshot( void ) const;
	void generateTriangles(
		PackedVector3Array &arrVertices, PackedVector2Array *pUvs, const int32_t nLod,
		const T3DData::t3dheightFilter_t filter, const bool require_nav,
		const AABB &rGlobalAabb
	) const;
	void generateTrianglePair(
		t3dtriangleBuffer_t &rBuffer, const bool bGenerateUvs, const int32_t nLod,
		const T3DData::t3dheightFilter_t filter, const bool require_nav, const int32_t x,
		const int32_t z, const t3dbakeSnapshot_t *pSnapshot = nullptr
	) const;

public:
	static constexpr int64_t NATIVE_API_REVISION = 2026081802LL;
	static constexpr const char *NATIVE_BUILD_ID = "a2.8.6-memory-zone";
	static t3ddebugLevel_t s_debugLevel; // Initialized in terrain_3d.cpp

	T3DTerrain( void );
	~T3DTerrain( void );
	bool isInsideWorld( void ) const {
		return m_bIsInsideWorld;
	}
	bool isInitialized( void ) const {
		return m_bInitialized;
	}

	// Terrain
	String getVersion( void ) const {
		return m_version;
	}
	int64_t getNativeApiRevision( void ) const {
		return NATIVE_API_REVISION;
	}
	String getNativeBuildId( void ) const {
		return String( NATIVE_BUILD_ID );
	}
	void setDebugLevel( const t3ddebugLevel_t level );
	t3ddebugLevel_t getDebugLevel( void ) const {
		return s_debugLevel;
	}
	void setDataDirectory( String dir );
	String getDataDirectory( void ) const {
		return m_pData ? m_dataDirectory : "";
	}

	// Object references
	T3DData *getData( void ) const {
		return m_pData;
	}
	void setAssets( const Ref<T3DAssets> &rAssets );
	Ref<T3DAssets> getAssets( void ) const {
		return m_pAssets;
	}
	void setAutoShaderProfile( const Ref<Resource> &rProfile );
	Ref<Resource> getAutoShaderProfile( void ) const {
		return m_pAutoShaderProfile;
	}
	T3DCollision *getCollision( void ) const {
		return m_pCollision;
	}
	T3DInstancer *getInstancer( void ) const {
		return m_pInstancer;
	}
	T3DNodeStreamer *getNodeStreamer( void ) const {
		return m_pNodeStreamer;
	}
	T3DLightmapStreamer *getLightmapStreamer( void ) const {
		return m_pLightmapStreamer;
	}
	T3DHLODStreamer *getHlodStreamer( void ) const { return m_pHlodStreamer; }
	void setEditor( T3DEditor *pEditor );
	T3DEditor *getEditor( void ) const {
		return m_pEditor;
	}
	void setPlugin( Object *pPlugin );
	Object *getPlugin( void ) const {
		return m_pEditorPlugin;
	}

	// Regions
	void setRegionSize( const t3dregionSize_t size );
	t3dregionSize_t getRegionSize( void ) const {
		return m_nRegionSize;
	}
	void changeRegionSize( const t3dregionSize_t size ) {
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
	bool getSave16Bit( void ) const {
		return m_bSave16Bit;
	}
	void setPerformanceTarget( const t3dperformanceTarget_t target );
	t3dperformanceTarget_t getPerformanceTarget( void ) const {
		return m_performanceTarget;
	}
	t3dperformanceProfileStatus_t getPerformanceProfileStatus( void ) const {
		return m_performanceProfileStatus;
	}
	String getPerformanceProfileStatusName( void ) const;
	int getPerformanceProfileVersion( void ) const;
	String getPerformanceProfileSignature( void ) const;
	Dictionary getPerformanceProfileManifest( void ) const;
	Array getPerformanceProfileDiff( void ) const;
	void resetPerformanceProfile( void );
	void setEditorExecutionMode( t3deditorExecutionMode_t mode );
	t3deditorExecutionMode_t getEditorExecutionMode( void ) const {
		return m_editorExecutionMode;
	}
	void setEditorRuntimeStreamingPreview( bool bEnabled );
	bool isEditorRuntimeStreamingPreview( void ) const {
		return m_editorExecutionMode == EDITOR_EXECUTION_RUNTIME_PROFILE;
	}
	void setEditorNodeStreamingPreview( bool bEnabled );
	bool isEditorNodeStreamingPreviewEnabled( void ) const {
		return m_bEditorNodeStreamingPreview;
	}
	bool isEditorNodeStreamingPreviewActive( void ) const {
		return isEditorAuthoringModeInternal() && m_bEditorNodeStreamingPreview;
	}
	bool isEditorAuthoringMode( void ) const {
		return isEditorAuthoringModeInternal();
	}
	bool isAuthoringResidencyReadbackSafe( void ) const;
	bool isGpuRegionCacheEffective( void ) const {
		return effectiveGpuRegionCacheEnabled();
	}
	bool isCpuRegionCacheEffective( void ) const {
		return effectiveCpuRegionCacheEnabled();
	}
	bool isInstanceRegionCacheEffective( void ) const {
		return effectiveInstanceRegionCacheEnabled();
	}
	bool isInstancerCellStreamingEffective( void ) const {
		return effectiveInstancerCellStreamingEnabled();
	}
	bool isDynamicNodeStreamingEffective( void ) const {
		return effectiveDynamicNodeStreamingEnabled();
	}
	void setStreamingActiveRadius( int nRadius );
	int getStreamingActiveRadius( void ) const { return m_regionStreamingConfig.activeRadius; }
	void setStreamingHotRadius( int nRadius );
	int getStreamingHotRadius( void ) const { return m_regionStreamingConfig.hotRadius; }
	void setStreamingColdRadius( int nRadius );
	int getStreamingColdRadius( void ) const { return m_regionStreamingConfig.coldRadius; }
	void setStreamingUnloadGuard( int nRegions );
	int getStreamingUnloadGuard( void ) const { return m_regionStreamingConfig.unloadGuard; }
	void setStreamingPredictionEnabled( bool bEnabled );
	bool isStreamingPredictionEnabled( void ) const { return m_regionStreamingConfig.bPredictionEnabled; }
	void setStreamingPredictionLookahead( real_t fSeconds );
	real_t getStreamingPredictionLookahead( void ) const { return ( real_t ) ( m_regionStreamingConfig.fPredictionLookaheadSeconds ); }
	void setStreamingPredictionMaximumRegions( int nRegions );
	int getStreamingPredictionMaximumRegions( void ) const { return m_regionStreamingConfig.predictionMaximumRegions; }
	t3dregionStreamingState_t getRegionStreamingStateInternal( const Vector2i &vLocation ) const;
	bool isRegionStreamingPrefetchInternal( const Vector2i &vLocation ) const;
	int getRegionStreamingState( const Vector2i &vLocation ) const { return ( int ) ( getRegionStreamingStateInternal( vLocation ) ); }
	bool isRegionStreamingPrefetch( const Vector2i &vLocation ) const { return isRegionStreamingPrefetchInternal( vLocation ); }
	Dictionary getRegionStreamingStats( void ) const;
	Dictionary getRegionStreamingConfigurationStatus( void ) const;
	Dictionary getHeightSourceDiagnostics( const Vector3 &rWorldPosition ) const;
	uint64_t getRegionStreamingGeneration( void ) const { return m_nRegionStreamingGeneration; }
	Vector2i getRegionStreamingCenterInternal( void ) const { return m_regionStreamingCenter; }
	Vector2i getRegionStreamingPredictedCenterInternal( void ) const { return m_regionStreamingPredictedCenter; }

	bool isRuntimeShaderTieringActive( void ) const {
		// Authoring deliberately keeps the complete Near shader on every clipmap level.
		// Runtime Profile and exported runtime exercise the compact Near/Middle/Far layout.
		return m_bRuntimeShaderTieringEnabled && !isEditorAuthoringModeInternal();
	}
	Dictionary getMemoryEstimate( void ) const;
	Dictionary getTrackedCurrentResidency( void ) const;
	Dictionary getMemoryBudgetStats( void ) const;
	void setTotalMemoryBudgetMb( int nBudgetMb );
	int getTotalMemoryBudgetMb( void ) const { return m_nTotalMemoryBudgetMb; }
	void setMemoryTransientReserveMb( int nReserveMb );
	int getMemoryTransientReserveMb( void ) const { return m_nMemoryTransientReserveMb; }
	void setGpuRegionCacheBudgetAuto( bool bEnabled );
	bool isGpuRegionCacheBudgetAuto( void ) const { return m_bGpuRegionCacheBudgetAuto; }
	void setCpuRegionCacheBudgetAuto( bool bEnabled );
	bool isCpuRegionCacheBudgetAuto( void ) const { return m_bCpuRegionCacheBudgetAuto; }
	void setInstanceRegionCacheBudgetAuto( bool bEnabled );
	bool isInstanceRegionCacheBudgetAuto( void ) const { return m_bInstanceRegionCacheBudgetAuto; }
	void setLightmapResidencyBudgetAuto( bool bEnabled );
	bool isLightmapResidencyBudgetAuto( void ) const { return m_bLightmapResidencyBudgetAuto; }
	void setLightmapResidencyBudgetMb( int nBudgetMb );
	int getLightmapResidencyBudgetMb( void ) const { return m_nLightmapResidencyBudgetMb; }
	void setHlodMemoryBudgetAuto( bool bEnabled );
	bool isHlodMemoryBudgetAuto( void ) const { return m_bHlodMemoryBudgetAuto; }
	void setShaderCacheBudgetAuto( bool bEnabled );
	bool isShaderCacheBudgetAuto( void ) const { return m_bShaderCacheBudgetAuto; }
	int getEffectiveGpuRegionCacheBudgetMb( void ) const { return effectiveGpuRegionCacheEnabled() ? m_nEffectiveGpuRegionCacheBudgetMb : 0; }
	int getEffectiveCpuRegionCacheBudgetMb( void ) const { return effectiveCpuRegionCacheEnabled() ? m_nEffectiveCpuRegionCacheBudgetMb : 0; }
	int getEffectiveInstanceRegionCacheBudgetMb( void ) const { return effectiveInstanceRegionCacheEnabled() ? m_nEffectiveInstanceRegionCacheBudgetMb : 0; }
	int getEffectiveLightmapResidencyBudgetMb( void ) const { return m_bLightmapStreamingEnabled ? m_nEffectiveLightmapResidencyBudgetMb : 0; }
	int getEffectiveHlodMemoryBudgetMb( void ) const { return m_bHlodEnabled ? m_nEffectiveHlodMemoryBudgetMb : 0; }
	int getEffectiveShaderCacheBudgetMb( void ) const { return m_nEffectiveShaderCacheBudgetMb; }
	int64_t getMemoryBudgetPolicyGeneration( void ) const { return ( int64_t ) ( m_nMemoryBudgetPolicyGeneration ); }
	bool isMemoryBudgetOvercommitted( void ) const { return m_nMemoryBudgetResult.bHardOvercommit; }
	int64_t getMemoryBudgetHeadroomBytes( void ) const { return ( int64_t ) ( m_nMemoryBudgetResult.nHeadroomBytes ); }
	void forceMemoryBudgetRebalance( void );
	Dictionary getRuntimeDiagnostics( bool bDeep = false ) const;
	Dictionary getRuntimeParitySnapshot( void ) const;
	Ref<T3DMemoryStats> getMemoryStatsSnapshot( void ) const;
	TypedArray<T3DShaderTierStats> getShaderTierStatsSnapshot( void ) const;
	TypedArray<T3DCacheStats> getCacheStatsSnapshot( bool bDeep = false ) const;
	Ref<T3DFrameStats> getFrameStatsSnapshot( void ) const;
	Array getRuntimeProfileHistory( int nMaxSamples = 600 ) const;
	Array getRuntimeProfileHistorySince( int64_t nAfterSequence, int nMaxSamples = 600 ) const;
	void resetRuntimeDiagnostics( void );
	void setRuntimeProfilingEnabled( bool bEnabled );
	bool isRuntimeProfilingEnabled( void ) const { return m_bRuntimeProfilingEnabled; }
	void setRuntimeAdaptiveWorkBudgetsEnabled( bool bEnabled );
	bool isRuntimeAdaptiveWorkBudgetsEnabled( void ) const { return m_bRuntimeAdaptiveWorkBudgetsEnabled; }
	void setRuntimeAdaptiveTargetMs( real_t fTargetMs );
	real_t getRuntimeAdaptiveTargetMs( void ) const { return ( real_t ) ( m_nRuntimeAdaptiveTargetUsec ) / 1000.0f; }
	void setRuntimeAdaptiveHardMaxMs( real_t fHardMaxMs );
	real_t getRuntimeAdaptiveHardMaxMs( void ) const { return ( real_t ) ( m_nRuntimeAdaptiveHardMaxUsec ) / 1000.0f; }
	void setRuntimeAdaptiveAdjustmentWindowFrames( int nFrames );
	int getRuntimeAdaptiveAdjustmentWindowFrames( void ) const { return m_nRuntimeAdaptiveAdjustmentWindowFrames; }
	void setRuntimeAdaptiveMinGpuPages( int nPages );
	int getRuntimeAdaptiveMinGpuPages( void ) const { return m_nRuntimeAdaptiveMinGpuPages; }
	void setRuntimeAdaptiveMinCpuPages( int nPages );
	int getRuntimeAdaptiveMinCpuPages( void ) const { return m_nRuntimeAdaptiveMinCpuPages; }
	void setRuntimeAdaptiveMinInstancePages( int nPages );
	int getRuntimeAdaptiveMinInstancePages( void ) const { return m_nRuntimeAdaptiveMinInstancePages; }
	void setRuntimeAdaptiveMinInstancerCells( int nCells );
	int getRuntimeAdaptiveMinInstancerCells( void ) const { return m_nRuntimeAdaptiveMinInstancerCells; }
	void setRuntimeAdaptiveMinNodeLoadRegions( int nRegions );
	int getRuntimeAdaptiveMinNodeLoadRegions( void ) const { return m_nRuntimeAdaptiveMinNodeLoadRegions; }
	void setRuntimeAdaptiveMinNodeUnloadRegions( int nRegions );
	int getRuntimeAdaptiveMinNodeUnloadRegions( void ) const { return m_nRuntimeAdaptiveMinNodeUnloadRegions; }
	Dictionary getRuntimeAdaptiveBudgetStats( void ) const;
	void setRuntimeBoundaryWorkPhasingEnabled( bool bEnabled );
	bool isRuntimeBoundaryWorkPhasingEnabled( void ) const { return m_bRuntimeBoundaryWorkPhasingEnabled; }
	void setRuntimeBoundaryNodeDelayFrames( int nFrames );
	int getRuntimeBoundaryNodeDelayFrames( void ) const { return m_nRuntimeBoundaryNodeDelayFrames; }
	void setRuntimeBoundaryInstancerDelayFrames( int nFrames );
	int getRuntimeBoundaryInstancerDelayFrames( void ) const { return m_nRuntimeBoundaryInstancerDelayFrames; }
	Dictionary getRuntimeBoundaryPhasingStats( void ) const;
	void setGpuRegionCacheEnabled( bool bEnabled );
	bool isGpuRegionCacheEnabled( void ) const {
		return m_bGpuRegionCacheEnabled;
	}
	void setGpuRegionCacheBudgetMb( int nBudgetMb );
	int getGpuRegionCacheBudgetMb( void ) const {
		return m_nGpuRegionCacheBudgetMb;
	}
	void setGpuRegionUploadBudgetLayers( int nLayers );
	int getGpuRegionUploadBudgetLayers( void ) const {
		return m_nGpuRegionUploadBudgetLayers;
	}
	void setGpuRegionUploadBudgetMb( int nBudgetMb );
	int getGpuRegionUploadBudgetMb( void ) const {
		return m_nGpuRegionUploadBudgetMb;
	}
	void setGpuRegionSparePages( int nPages );
	int getGpuRegionSparePages( void ) const {
		return m_nGpuRegionSparePages;
	}
	void setGpuRegionRetireFrames( int nFrames );
	int getGpuRegionRetireFrames( void ) const {
		return m_nGpuRegionRetireFrames;
	}
	void setGpuRegionDetailLods( int nLods );
	int getGpuRegionDetailLods( void ) const {
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
	int getGpuRegionGuardRegions( void ) const {
		return m_nGpuRegionGuardRegions;
	}
	void setGpuRegionPrefetchRows( int nRows );
	int getGpuRegionPrefetchRows( void ) const {
		return m_nGpuRegionPrefetchRows;
	}
	void setGpuRegionHysteresisTicks( int nTicks );
	int getGpuRegionHysteresisTicks( void ) const {
		return m_nGpuRegionHysteresisTicks;
	}
	void setGpuRegionColorResolutionDivisor( int nDivisor );
	int getGpuRegionColorResolutionDivisor( void ) const {
		return m_nGpuRegionColorResolutionDivisor;
	}
	void setGpuRegionDerivedResolutionDivisor( int nDivisor );
	int getGpuRegionDerivedResolutionDivisor( void ) const {
		return m_nGpuRegionDerivedResolutionDivisor;
	}
	void setGpuMiddleHeightResolutionDivisor( int nDivisor );
	int getGpuMiddleHeightResolutionDivisor( void ) const { return m_nGpuMiddleHeightResolutionDivisor; }
	void setEditorGpuRegionCachePages( int nPages );
	int getEditorGpuRegionCachePages( void ) const { return m_nEditorGpuRegionCachePages; }
	void setRuntimeMaterialStreamingEnabled( bool bEnabled );
	bool isRuntimeMaterialStreamingEnabled( void ) const { return m_bRuntimeMaterialStreamingEnabled; }
	void setRuntimeMaterialCacheLayers( int nLayers );
	int getRuntimeMaterialCacheLayers( void ) const { return m_nRuntimeMaterialCacheLayers; }
	void setRuntimeMaterialUploadBudgetLayers( int nLayers );
	int getRuntimeMaterialUploadBudgetLayers( void ) const { return m_nRuntimeMaterialUploadBudgetLayers; }
	void setRuntimeReleaseSourceMaterialTextures( bool bEnabled );
	bool isRuntimeReleaseSourceMaterialTextures( void ) const { return m_bRuntimeReleaseSourceMaterialTextures; }
	void setGpuFarQualityDivisor( int nDivisor );
	int getGpuFarQualityDivisor( void ) const {
		return m_nGpuFarQualityDivisor;
	}
	void setFarMaterialResolution( int nResolution );
	int getFarMaterialResolution( void ) const { return m_nFarMaterialResolution; }
	void setFarControlResolution( int nResolution );
	int getFarControlResolution( void ) const { return m_nFarControlResolution; }
	int getGpuFarHeightAtlasSide( void ) const;
	int getGpuFarSurfaceAtlasSide( void ) const;
	int getGpuFarAtlasSide( void ) const { return getGpuFarHeightAtlasSide(); }
	uint64_t getGpuFarAtlasBytes( void ) const;
	void setGpuRegionCookedPagesEnabled( bool bEnabled );
	bool isGpuRegionCookedPagesEnabled( void ) const {
		return m_bGpuRegionCookedPagesEnabled;
	}
	void setGpuRegionCookOnSave( bool bEnabled );
	bool isGpuRegionCookOnSave( void ) const {
		return m_bGpuRegionCookOnSave;
	}
	Dictionary getGpuRegionCacheStats( void ) const {
		return m_pData ? m_pData->getGpuRegionCacheStats() : Dictionary();
	}
	void setCpuRegionCacheEnabled( bool bEnabled );
	bool isCpuRegionCacheEnabled( void ) const {
		return m_bCpuRegionCacheEnabled;
	}
	void setCpuRegionCacheBudgetMb( int nBudgetMb );
	int getCpuRegionCacheBudgetMb( void ) const {
		return m_nCpuRegionCacheBudgetMb;
	}
	void setCpuRegionLoadBudgetLayers( int nLayers );
	int getCpuRegionLoadBudgetLayers( void ) const {
		return m_nCpuRegionLoadBudgetLayers;
	}
	void setCpuRegionLoadBudgetMb( int nBudgetMb );
	int getCpuRegionLoadBudgetMb( void ) const {
		return m_nCpuRegionLoadBudgetMb;
	}
	Dictionary getCpuRegionCacheStats( void ) const {
		return m_pData ? m_pData->getCpuRegionCacheStats() : Dictionary();
	}
	void setInstanceRegionCacheEnabled( bool bEnabled );
	bool isInstanceRegionCacheEnabled( void ) const { return m_bInstanceRegionCacheEnabled; }
	void setInstanceRegionCacheBudgetMb( int nBudgetMb );
	int getInstanceRegionCacheBudgetMb( void ) const { return m_nInstanceRegionCacheBudgetMb; }
	void setInstanceRegionLoadBudgetPages( int nPages );
	int getInstanceRegionLoadBudgetPages( void ) const { return m_nInstanceRegionLoadBudgetPages; }
	void setInstanceRegionLoadBudgetMb( int nBudgetMb );
	int getInstanceRegionLoadBudgetMb( void ) const { return m_nInstanceRegionLoadBudgetMb; }
	Dictionary getInstanceRegionCacheStats( void ) const {
		return m_pData ? m_pData->getInstanceRegionCacheStats() : Dictionary();
	}
	void setInstancerCellSize( int nCellSize );
	int getInstancerCellSize( void ) const { return m_nInstancerCellSize; }
	void setInstancerCellStreamingEnabled( bool bEnabled );
	bool isInstancerCellStreamingEnabled( void ) const { return m_bInstancerCellStreamingEnabled; }
	void setInstancerSingleLodBuffers( bool bEnabled );
	bool isInstancerSingleLodBuffers( void ) const { return m_bInstancerSingleLodBuffers; }
	void setInstancerCellStreamingMargin( real_t fMargin );
	real_t getInstancerCellStreamingMargin( void ) const { return m_fInstancerCellStreamingMargin; }
	void setInstancerCellStreamingUpdateDistance( real_t fDistance );
	real_t getInstancerCellStreamingUpdateDistance( void ) const { return m_fInstancerCellStreamingUpdateDistance; }
	void setInstancerUpdateCellBudget( int nBudget );
	int getInstancerUpdateCellBudget( void ) const { return m_nInstancerUpdateCellBudget; }
	void setDynamicNodeStreamingEnabled( bool bEnabled );
	bool isDynamicNodeStreamingEnabled( void ) const { return m_bDynamicNodeStreamingEnabled; }
	void setDynamicNodeStreamRadius( int nRadius );
	int getDynamicNodeStreamRadius( void ) const { return m_nDynamicNodeStreamRadius; }
	void setDynamicNodeLoadRegionBudget( int nBudget );
	int getDynamicNodeLoadRegionBudget( void ) const { return m_nDynamicNodeLoadRegionBudget; }
	void setDynamicNodeLoadNodeBudget( int nBudget );
	int getDynamicNodeLoadNodeBudget( void ) const { return m_nDynamicNodeLoadNodeBudget; }
	void setDynamicNodeUnloadRegionBudget( int nBudget );
	int getDynamicNodeUnloadRegionBudget( void ) const { return m_nDynamicNodeUnloadRegionBudget; }
	void setLightmapStreamingEnabled( bool bEnabled );
	bool isLightmapStreamingEnabled( void ) const { return m_bLightmapStreamingEnabled; }
	void setLightmapStreamRadius( int nRadius );
	int getLightmapStreamRadius( void ) const { return m_nLightmapStreamRadius; }
	void setLightmapLoadBudget( int nBudget );
	int getLightmapLoadBudget( void ) const { return m_nLightmapLoadBudget; }
	void setLightmapUnloadBudget( int nBudget );
	int getLightmapUnloadBudget( void ) const { return m_nLightmapUnloadBudget; }
	Dictionary getLightmapStreamingStats( void ) const;
	void setRegionLightmapOutputDirectory( const String &rDirectory );
	String getRegionLightmapOutputDirectory( void ) const { return m_regionLightmapOutputDirectory; }
	void setRegionLightmapProxyLod( int nLod );
	int getRegionLightmapProxyLod( void ) const { return m_nRegionLightmapProxyLod; }
	void setRegionLightmapContextRadius( int nRadius );
	int getRegionLightmapContextRadius( void ) const { return m_nRegionLightmapContextRadius; }
	void setRegionLightmapTexelSize( real_t fTexelSize );
	real_t getRegionLightmapTexelSize( void ) const { return m_fRegionLightmapTexelSize; }
	void setRegionLightmapBakeQuality( int nQuality );
	int getRegionLightmapBakeQuality( void ) const { return m_nRegionLightmapBakeQuality; }
	void setRegionLightmapShadowmaskMode( int nMode );
	int getRegionLightmapShadowmaskMode( void ) const { return m_nRegionLightmapShadowmaskMode; }
	void setHlodEnabled( bool bEnabled );
	bool isHlodEnabled( void ) const { return m_bHlodEnabled; }
	void setHlodManifestPath( const String &rPath );
	String getHlodManifestPath( void ) const { return m_hlodManifestPath; }
	void setHlodTargetErrorPixels( real_t fPixels );
	real_t getHlodTargetErrorPixels( void ) const { return m_fHlodTargetErrorPixels; }
	void setHlodHysteresis( real_t fRatio );
	real_t getHlodHysteresis( void ) const { return m_fHlodHysteresis; }
	void setHlodMaxLevel( int nLevel ); int getHlodMaxLevel( void ) const { return m_nHlodMaxLevel; }
	void setHlodLoadBudget( int nBudget ); int getHlodLoadBudget( void ) const { return m_nHlodLoadBudget; }
	void setHlodUnloadBudget( int nBudget ); int getHlodUnloadBudget( void ) const { return m_nHlodUnloadBudget; }
	void setHlodMemoryBudgetMb( int nBudgetMb ); int getHlodMemoryBudgetMb( void ) const { return m_nHlodMemoryBudgetMb; }
	void setHlodMaxShadowLevel( int nLevel ); int getHlodMaxShadowLevel( void ) const { return m_nHlodMaxShadowLevel; }
	void setHlodForcedDebugLevel( int nLevel ); int getHlodForcedDebugLevel( void ) const { return m_nHlodForcedDebugLevel; }
	void setEditorHlodPreview( bool bEnabled );
	bool isEditorHlodPreviewEnabled( void ) const { return m_bEditorHlodPreview; }
	bool isEditorHlodPreviewActive( void ) const { return isEditorAuthoringModeInternal() && m_bEditorHlodPreview; }
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
	int64_t addStreamedScene(
		const Ref<PackedScene> &rScene, const Transform3D &rGlobalTransform,
		const Dictionary &rState = Dictionary(), int nFlags = 0, int nHlodMinLevel = 1
	);
	int64_t addStreamedScenePath(
		const String &rScenePath, const Transform3D &rGlobalTransform,
		const Dictionary &rState = Dictionary(), int nFlags = 0, int nHlodMinLevel = 1
	);
	bool removeStreamedScene( int64_t nSpawnId );
	int allocateStreamOwnerId( void );
	void setNextStreamOwnerId( int nOwnerId );
	int getNextStreamOwnerId( void ) const { return m_nNextStreamOwnerId; }
	Dictionary getNodeStreamingStats( void ) const;
	void setLabelDistance( const real_t fDistance );
	real_t getLabelDistance( void ) const {
		return m_fLabelDistance;
	}
	void setLabelSize( const int nSize );
	int getLabelSize( void ) const {
		return m_nLabelSize;
	}
	void updateRegionLabels( void );

	// Target Tracking
	void setCamera( Camera3D *pCamera );
	Camera3D *getCamera( void ) const {
		return cast_to<Camera3D>( m_camera.ptr() );
	}
	void setClipmapTarget( Node3D *pNode );
	Node3D *getClipmapTarget( void ) const {
		return m_clipmapTarget.ptr();
	}
	Vector3 getClipmapTargetPosition( void ) const;
	void setCollisionTarget( Node3D *pNode );
	Node3D *getCollisionTarget( void ) const {
		return m_collisionTarget.ptr();
	}
	Vector3 getCollisionTargetPosition( void ) const;
	void setLightTarget( Node3D *pNode );
	void setSemanticShadowPolicyEnabled( bool bEnabled );
	bool isSemanticShadowPolicyEnabled( void ) const { return m_bSemanticShadowPolicyEnabled; }
	void setEnvironmentShadowQuality( t3denvironmentShadowQuality_t eQuality );
	t3denvironmentShadowQuality_t getEnvironmentShadowQuality( void ) const { return m_environmentShadowQuality; }
	void setEnvironmentShadowExternalMask( uint32_t nMask );
	uint32_t getEnvironmentShadowExternalMask( void ) const { return m_nEnvironmentShadowExternalMask; }
	void setSemanticDirectionalReceptionEnabled( bool bEnabled );
	bool isSemanticDirectionalReceptionEnabled( void ) const { return m_bSemanticDirectionalReceptionEnabled; }
	void setEnvironmentDirectionalExternalMask( uint32_t nMask );
	uint32_t getEnvironmentDirectionalExternalMask( void ) const { return m_nEnvironmentDirectionalExternalMask; }
	Node3D *getLightTarget( void ) const {
		return m_lightTarget.ptr();
	}
	void snap( void );

	// Collision Aliases
	void setCollisionMode( const t3dcollisionMode_t mode ) {
		m_pCollision ? m_pCollision->setMode( mode ) : ( void ) 0;
	}
	t3dcollisionMode_t getCollisionMode( void ) const {
		return m_pCollision ? m_pCollision->getMode() : t3dcollisionMode_t::DYNAMIC_GAME;
	}
	void setCollisionShapeSize( const uint16_t nSize ) {
		m_pCollision ? m_pCollision->setShapeSize( nSize ) : ( void ) 0;
	}
	uint16_t getCollisionShapeSize( void ) const {
		return m_pCollision ? m_pCollision->getShapeSize() : 16;
	}
	void setCollisionRadius( const uint16_t nRadius ) {
		m_pCollision ? m_pCollision->setRadius( nRadius ) : ( void ) 0;
	}
	uint16_t getCollisionRadius( void ) const {
		return m_pCollision ? m_pCollision->getRadius() : 64;
	}
	void setCollisionLayer( const uint32_t nLayers ) {
		m_pCollision ? m_pCollision->setLayer( nLayers ) : ( void ) 0;
	}
	uint32_t getCollisionLayer( void ) const {
		return m_pCollision ? m_pCollision->getLayer() : 1;
	}
	void setCollisionMask( const uint32_t nMask ) {
		m_pCollision ? m_pCollision->setMask( nMask ) : ( void ) 0;
	}
	uint32_t getCollisionMask( void ) const {
		return m_pCollision ? m_pCollision->getMask() : 1;
	}
	void setCollisionPriority( const real_t fPriority ) {
		m_pCollision ? m_pCollision->setPriority( fPriority ) : ( void ) 0;
	}
	real_t getCollisionPriority( void ) const {
		return m_pCollision ? m_pCollision->getPriority() : 1.f;
	}
	void setPhysicsMaterial( const Ref<PhysicsMaterial> &rMat ) {
		m_pCollision ? m_pCollision->setPhysicsMaterial( rMat ) : ( void ) 0;
	}
	Ref<PhysicsMaterial> getPhysicsMaterial( void ) const {
		return m_pCollision ? m_pCollision->getPhysicsMaterial() : Ref<PhysicsMaterial>();
	}

	// Terrain Mesh
	T3DMesher *getMesher( void ) const {
		return m_pTerrainMesher;
	}
	void setMaterial( const Ref<T3DMaterial> &rMaterial );
	Ref<T3DMaterial> getMaterial( void ) const {
		return m_pMaterial;
	}
	void setMeshLods( const int nCount );
	int getMeshLods( void ) const {
		return m_nMeshLods;
	}
	void setRuntimeShaderTieringEnabled( bool bEnabled );
	bool isRuntimeShaderTieringEnabled( void ) const { return m_bRuntimeShaderTieringEnabled; }
	void setRuntimeNearShaderLods( int nCount );
	int getRuntimeNearShaderLods( void ) const { return m_nRuntimeNearShaderLods; }
	void setRuntimeMiddleShaderLods( int nCount );
	int getRuntimeMiddleShaderLods( void ) const { return m_nRuntimeMiddleShaderLods; }
	void setRuntimeNearRangeEndM( real_t fRangeM );
	real_t getRuntimeNearRangeEndM( void ) const;
	void setRuntimeMiddleRangeEndM( real_t fRangeM );
	real_t getRuntimeMiddleRangeEndM( void ) const;
	real_t getRuntimeShadowRangeEndM( void ) const { return lodCountToRange( m_nShadowLodCount ); }
	void setRuntimeSyncDetailLods( bool bEnabled );
	bool isRuntimeSyncDetailLods( void ) const { return m_bRuntimeSyncDetailLods; }
	void setRuntimeClipmapUpdateBudget( int nCount );
	int getRuntimeClipmapUpdateBudget( void ) const { return m_nRuntimeClipmapUpdateBudget; }
	void setRuntimePagePrepareThreads( int nCount );
	int getRuntimePagePrepareThreads( void ) const { return m_nRuntimePagePrepareThreads; }
	void setTessellationLevel( const int nLevel );
	int getTessellationLevel( void ) const {
		return m_tessellationLevel;
	}
	void setMeshSize( const int nSize );
	int getMeshSize( void ) const {
		return m_nMeshSize;
	}
	void setVertexSpacing( const real_t fSpacing );
	real_t getVertexSpacing( void ) const {
		return m_fVertexSpacing;
	}
	void setCullMargin( const real_t fMargin );
	real_t getCullMargin( void ) const {
		return m_fCullMargin;
	}
	void setCastShadows( const RenderingServer::ShadowCastingSetting castShadows );
	RenderingServer::ShadowCastingSetting getCastShadows( void ) const {
		return m_castShadows;
	}
	void setShadowLodCount( const int nCount );
	int getShadowLodCount( void ) const {
		return m_nShadowLodCount;
	}
	void setNearShadowEnabled( bool bEnabled );
	bool getNearShadowEnabled( void ) const { return m_bNearShadowEnabled; }
	void setMiddleShadowEnabled( bool bEnabled );
	bool getMiddleShadowEnabled( void ) const { return m_bMiddleShadowEnabled; }
	void setFarShadowEnabled( bool bEnabled );
	bool getFarShadowEnabled( void ) const { return m_bFarShadowEnabled; }
	void setNearShadowLodCount( int nCount );
	int getNearShadowLodCount( void ) const { return m_nNearShadowLodCount; }
	void setMiddleShadowLodCount( int nCount );
	int getMiddleShadowLodCount( void ) const { return m_nMiddleShadowLodCount; }
	void setFarShadowLodCount( int nCount );
	int getFarShadowLodCount( void ) const { return m_nFarShadowLodCount; }
	void setGiMode( const GeometryInstance3D::GIMode giMode );
	GeometryInstance3D::GIMode getGiMode( void ) const {
		return m_giMode;
	}
	void setRenderLayers( const uint32_t nLayers );
	uint32_t getRenderLayers( void ) const {
		return m_renderLayers;
	}

	// Material Displacement Aliases
	void setDisplacementScale( const real_t fDisplacementScale ) {
		m_pMaterial.is_valid() ? m_pMaterial->setDisplacementScale( fDisplacementScale ) : ( void ) 0;
	}
	real_t getDisplacementScale( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getDisplacementScale() : 1.f;
	}
	void setDisplacementSharpness( const real_t fDisplacementSharpness ) {
		m_pMaterial.is_valid() ? m_pMaterial->setDisplacementSharpness( fDisplacementSharpness ) : ( void ) 0;
	}
	real_t getDisplacementSharpness( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getDisplacementSharpness() : 0.25f;
	}
	void setBufferShaderOverrideEnabled( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setBufferShaderOverrideEnabled( bEnabled ) : ( void ) 0;
	}
	bool isBufferShaderOverrideEnabled( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->isBufferShaderOverrideEnabled() : false;
	}
	void setBufferShaderOverride( const Ref<Shader> &rShader ) {
		return m_pMaterial.is_valid() ? m_pMaterial->setBufferShaderOverride( rShader ) : ( void ) 0;
	}
	Ref<Shader> getBufferShaderOverride( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getBufferShaderOverride() : Ref<Shader>();
	}

	// Ocean Mesh
	T3DMesher *getOceanMesher( void ) const {
		return m_pOceanMesher;
	}
	void setOceanEnabled( const bool bEnabled );
	bool isOceanEnabled( void ) const {
		return m_bOceanEnabled;
	}
	void setOceanMeshLods( const int nCount );
	int getOceanMeshLods( void ) const {
		return m_nOceanMeshLods;
	}
	void setOceanTessellationLevel( const int nLevel );
	int getOceanTessellationLevel( void ) const {
		return m_oceanTessellationLevel;
	}
	void setOceanMeshSize( const int nSize );
	int getOceanMeshSize( void ) const {
		return m_nOceanMeshSize;
	}
	void setOceanVertexSpacing( const real_t fSpacing );
	real_t getOceanVertexSpacing( void ) const {
		return m_fOceanVertexSpacing;
	}
	void setOceanCullMargin( const real_t fMargin );
	real_t getOceanCullMargin( void ) const {
		return m_fOceanCullMargin;
	}
	void setOceanCastShadows( const RenderingServer::ShadowCastingSetting castShadows );
	RenderingServer::ShadowCastingSetting getOceanCastShadows( void ) const {
		return m_oceanCastShadows;
	}
	void setOceanGiMode( const GeometryInstance3D::GIMode giMode );
	GeometryInstance3D::GIMode getOceanGiMode( void ) const {
		return m_oceanGiMode;
	}
	void setOceanRenderLayers( const uint32_t nLayers );
	uint32_t getOceanRenderLayers( void ) const {
		return m_oceanRenderLayers;
	}
	void setOceanPerformancePreset( const t3doceanPerformancePreset_t preset );
	t3doceanPerformancePreset_t getOceanPerformancePreset( void ) const {
		return m_oceanPerformancePreset;
	}
	void setOceanVertexWaveIterations( const int nIterations );
	int getOceanVertexWaveIterations( void ) const {
		return m_nOceanVertexWaveIterations;
	}
	void setOceanFragmentWaveIterations( const int nIterations );
	int getOceanFragmentWaveIterations( void ) const {
		return m_nOceanFragmentWaveIterations;
	}
	void setOceanRefractionMode( const t3doceanRefractionMode_t mode );
	t3doceanRefractionMode_t getOceanRefractionMode( void ) const {
		return m_oceanRefractionMode;
	}
	void setOceanFoamMode( const t3doceanFoamMode_t mode );
	t3doceanFoamMode_t getOceanFoamMode( void ) const {
		return m_oceanFoamMode;
	}
	void setOceanScatteringMode( const t3doceanScatteringMode_t mode );
	t3doceanScatteringMode_t getOceanScatteringMode( void ) const {
		return m_oceanScatteringMode;
	}
	void setOceanShadingMode( const t3doceanShadingMode_t mode );
	t3doceanShadingMode_t getOceanShadingMode( void ) const {
		return m_oceanShadingMode;
	}
	void setOceanMaterial( const Ref<Material> &rMaterial );
	Ref<Material> getOceanMaterial( void ) const {
		return m_pOceanMaterial;
	}

	// Rendering
	void setMouseLayer( const uint32_t nLayer );
	uint32_t getMouseLayer( void ) const {
		return m_mouseLayer;
	}
	void setFreeEditorTextures( const bool bFreeTextures ) {
		m_bFreeEditorTextures = bFreeTextures;
	}
	bool getFreeEditorTextures( void ) const {
		return m_bFreeEditorTextures;
	}
	void setInstancerMode( const t3dinstancerMode_t mode ) {
		m_pInstancer ? m_pInstancer->setMode( mode ) : ( void ) 0;
	}
	t3dinstancerMode_t getInstancerMode( void ) const {
		return m_pInstancer ? m_pInstancer->getMode() : t3dinstancerMode_t::NORMAL;
	}

	// Utility
	Vector3 getIntersection( const Vector3 &rSrcPos, const Vector3 &rDirection, const bool bGpuMode = false );
	Dictionary getRaycastResult(
		const Vector3 &rSrcPos, const Vector3 &rDirection,
		const uint32_t nColMask = 0xFFFFFFFF, const bool bExcludeSelf = false
	) const;
	Ref<Mesh> bakeMesh(
		const int nLod,
		const T3DData::t3dheightFilter_t filter = T3DData::HEIGHT_FILTER_NEAREST
	) const;
	Ref<Mesh> bakeRegionLightmapMesh(
		const Vector2i &rRegionLocation, int nLod = 2,
		int nContextRadiusRegions = 1, real_t fTexelSize = 1.0f
	) const;
	Ref<Mesh> bakeLightmapClusterMesh(
		const Vector2i &rRegionMin, const Vector2i &rRegionMax,
		int nLod = 2, int nContextRadiusRegions = 1, real_t fTexelSize = 1.0f
	) const;
	Ref<ArrayOccluder3D> bakeOccluder( const int nLod ) const;
	PackedVector3Array generateNavMeshSourceGeometry(
		const AABB &rGlobalAabb,
		const bool bRequireNav = true
	) const;

	// Warnings
	void setWarning( const uint8_t nWarning, const bool bEnabled );
	uint8_t getWarnings( void ) const {
		return m_nWarnings;
	}
	PackedStringArray _get_configuration_warnings( void ) const override;

	// Overlay Aliases
	void setShowRegionGrid( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowRegionGrid( bEnabled ) : ( void ) 0;
	}
	bool getShowRegionGrid( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowRegionGrid() : false;
	}
	void setShowInstancerGrid( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowInstancerGrid( bEnabled ) : ( void ) 0;
	}
	bool getShowInstancerGrid( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowInstancerGrid() : false;
	}
	void setShowVertexGrid( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowVertexGrid( bEnabled ) : ( void ) 0;
	}
	bool getShowVertexGrid( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowVertexGrid() : false;
	}
	void setShowContours( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowContours( bEnabled ) : ( void ) 0;
	}
	bool getShowContours( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowContours() : false;
	}
	void setShowNavigation( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowNavigation( bEnabled ) : ( void ) 0;
	}
	bool getShowNavigation( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowNavigation() : false;
	}

	void setComputeDisplacementEnabled( bool bEnabled );
	bool getComputeDisplacementEnabled( void ) const {
		return m_bComputeDisplacementEnabled.load( std::memory_order_acquire );
	}
	void setDisplacementUpdateDistance( const real_t fDistance );
	real_t getDisplacementUpdateDistance( void ) const {
		return m_fDisplacementUpdateDistance;
	}
	bool isComputeDisplacementActive( void ) const {
		return m_bComputeDisplacementActive.load( std::memory_order_acquire );
	}

	// Debug View Aliases
	void setShowCheckered( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowCheckered( bEnabled ) : ( void ) 0;
	}
	bool getShowCheckered( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowCheckered() : false;
	}
	void setShowGrey( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowGrey( bEnabled ) : ( void ) 0;
	}
	bool getShowGrey( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowGrey() : false;
	}
	void setShowHeightmap( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowHeightmap( bEnabled ) : ( void ) 0;
	}
	bool getShowHeightmap( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowHeightmap() : false;
	}
	void setShowJaggedness( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowJaggedness( bEnabled ) : ( void ) 0;
	}
	bool getShowJaggedness( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowJaggedness() : false;
	}
	void setShowAutoshader( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowAutoshader( bEnabled ) : ( void ) 0;
	}
	bool getShowAutoshader( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowAutoshader() : false;
	}
	void setShowControlTexture( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowControlTexture( bEnabled ) : ( void ) 0;
	}
	bool getShowControlTexture( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowControlTexture() : false;
	}
	void setShowControlBlend( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowControlBlend( bEnabled ) : ( void ) 0;
	}
	bool getShowControlBlend( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowControlBlend() : false;
	}
	void setShowControlAngle( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowControlAngle( bEnabled ) : ( void ) 0;
	}
	bool getShowControlAngle( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowControlAngle() : false;
	}
	void setShowControlScale( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowControlScale( bEnabled ) : ( void ) 0;
	}
	bool getShowControlScale( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowControlScale() : false;
	}
	void setShowColormap( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowColormap( bEnabled ) : ( void ) 0;
	}
	bool getShowColormap( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowColormap() : false;
	}
	void setShowRoughmap( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowRoughmap( bEnabled ) : ( void ) 0;
	}
	bool getShowRoughmap( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowRoughmap() : false;
	}
	void setShowDisplacementBuffer( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowDisplacementBuffer( bEnabled ) : ( void ) 0;
	}
	bool getShowDisplacementBuffer( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowDisplacementBuffer() : false;
	}

	// PBR View Aliases
	void setShowTextureAlbedo( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowTextureAlbedo( bEnabled ) : ( void ) 0;
	}
	bool getShowTextureAlbedo( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowTextureAlbedo() : false;
	}
	void setShowTextureHeight( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowTextureHeight( bEnabled ) : ( void ) 0;
	}
	bool getShowTextureHeight( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowTextureHeight() : false;
	}
	void setShowTextureNormal( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowTextureNormal( bEnabled ) : ( void ) 0;
	}
	bool getShowTextureNormal( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowTextureNormal() : false;
	}
	void setShowTextureRough( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowTextureRough( bEnabled ) : ( void ) 0;
	}
	bool getShowTextureRough( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowTextureRough() : false;
	}
	void setShowTextureAo( const bool bEnabled ) {
		m_pMaterial.is_valid() ? m_pMaterial->setShowTextureAo( bEnabled ) : ( void ) 0;
	}
	bool getShowTextureAo( void ) const {
		return m_pMaterial.is_valid() ? m_pMaterial->getShowTextureAo() : false;
	}

protected:
	void _notification( const int nWhat );
	void _validate_property( PropertyInfo &rProperty ) const;
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( T3DTerrain::t3dperformanceTarget_t );
VARIANT_ENUM_CAST( T3DTerrain::t3dperformanceProfileStatus_t );
VARIANT_ENUM_CAST( T3DTerrain::t3deditorExecutionMode_t );
VARIANT_ENUM_CAST( T3DTerrain::t3dregionSize_t );
VARIANT_ENUM_CAST( T3DTerrain::t3ddebugLevel_t );


VARIANT_ENUM_CAST( T3DTerrain::t3doceanPerformancePreset_t );
VARIANT_ENUM_CAST( T3DTerrain::t3doceanRefractionMode_t );
VARIANT_ENUM_CAST( T3DTerrain::t3doceanFoamMode_t );
VARIANT_ENUM_CAST( T3DTerrain::t3doceanScatteringMode_t );
VARIANT_ENUM_CAST( T3DTerrain::t3doceanShadingMode_t );

VARIANT_ENUM_CAST( T3DTerrain::t3denvironmentShadowQuality_t );

using Terrain3D = T3DTerrain;

#endif // TERRAIN3D_CLASS_H
