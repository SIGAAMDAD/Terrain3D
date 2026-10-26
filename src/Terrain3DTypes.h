// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_TYPES_H
#define TERRAIN3D_TYPES_H

// Central POD/data-contract authority. Keep behavior, ownership, and executable logic out of this header.
// Constants are a one-way dependency because fixed-size records and enum defaults consume T3D_* macros.
#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Callback signatures embedded in POD interfaces.
typedef void ( *t3dParallelTileTask_t )( void *, size_t, size_t, size_t, size_t, int32_t );

typedef void *( *t3dVTCookAllocateFn )( void *pUser, uint64_t nBytes, uint32_t nAlignment );
typedef void ( *t3dVTCookFreeFn )( void *pUser, void *pMemory );
typedef uint64_t ( *t3dVTCookClusterBoundFn )( void *pUser, uint32_t nCodec, uint64_t nDecodedBytes );
typedef bool ( *t3dVTCookClusterEncodeFn )( void *pUser,
        uint32_t nCodec,
        const uint8_t *pSource,
        uint32_t nSourceBytes,
        uint8_t *pDestination,
        uint32_t nDestinationCapacity,
        uint32_t &rWrittenBytes );
typedef void *( *t3dVTPackRuntimeAllocateFn_t )( void *pUser, size_t nBytes, size_t nAlignment );
typedef void ( *t3dVTPackRuntimeFreeFn_t )( void *pUser, void *pMemory );

typedef void ( *t3dWorldReleaseConvertedSceneColdFn_t )( void *pUser );
typedef bool ( *t3dWorldIsConvertedSceneReadyColdFn_t )( void *pUser );
typedef bool ( *t3dWorldResolveMaterialVTColdFn_t )( void *pUser, uint64_t nStableMaterialKey, uint32_t &rAllocationId );

// Enumerations. Dependency-ordered so enum-valued capacities/defaults are declared before consumers.
// From src/Terrain3DConstants.h
typedef enum t3dPerformanceProfileOwner_e : uint8_t {
	T3D_PROFILE_OWNER_TERRAIN,
	T3D_PROFILE_OWNER_MATERIAL,
} t3dPerformanceProfileOwner_t;

// From src/Terrain3DConstants.h
typedef enum t3dPerformanceProfileValueKind_e : uint8_t {
	T3D_PROFILE_VALUE_INT,
	T3D_PROFILE_VALUE_FLOAT,
	T3D_PROFILE_VALUE_BOOL,
} t3dPerformanceProfileValueKind_t;

// From src/assets/resources/generated_texture.h
typedef enum t3dGeneratedTextureSourceRetentionPolicy_e : uint8_t {
	T3D_RETAIN_CPU_SOURCE = 0,
	T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD,
} t3dGeneratedTextureSourceRetentionPolicy_t;

// From src/assets/catalog/terrain_3d_assets.h
typedef enum t3dAssetType_e {
	TYPE_TEXTURE,
	TYPE_MESH,
} t3dAssetType_t;

// From src/assets/catalog/terrain_3d_assets.h
typedef enum t3dTexturePackingMode_e {
	TEXTURE_PACKING_LEGACY = 0,
	TEXTURE_PACKING_PACKED_PBR_R8 = 1,
} t3dTexturePackingMode_t;

// From src/assets/resources/terrain_3d_mesh_asset.h
typedef enum t3dInstanceCategory_e {
	INSTANCE_CATEGORY_CLUTTER = 0,
	INSTANCE_CATEGORY_GROUND_COVER = 1,
	INSTANCE_CATEGORY_OCCLUDER = 2,
} t3dInstanceCategory_t;

// From src/assets/resources/terrain_3d_mesh_asset.h
typedef enum t3dTerrainAlignment_e {
	TERRAIN_ALIGNMENT_UPRIGHT = 0,
	TERRAIN_ALIGNMENT_NORMAL = 1,
} t3dTerrainAlignment_t;

// From src/assets/resources/terrain_3d_mesh_asset.h
typedef enum t3dHLODContribution_e {
	HLOD_AUTO = 0,
	HLOD_INCLUDE = 1,
	HLOD_EXCLUDE = 2,
} t3dHLODContribution_t;

// From src/assets/resources/terrain_3d_mesh_asset.h
typedef enum t3dInstanceOcclusionPolicy_e {
	INSTANCE_OCCLUSION_NONE = 0,
	INSTANCE_OCCLUSION_BOX = 1,
	INSTANCE_OCCLUSION_EXPLICIT_MESH = 2,
} t3dInstanceOcclusionPolicy_t;

// From src/assets/resources/terrain_3d_mesh_asset.h
typedef enum t3dGenType_e {
	TYPE_NONE,
	TYPE_TEXTURE_CARD,
	MESH_TYPE_MAX,
} t3dGenType_t;

// From src/assets/resources/terrain_3d_packed_pbr_texture.h
typedef enum t3dSourceChannel_e {
	CHANNEL_RED = 0,
	CHANNEL_GREEN = 1,
	CHANNEL_BLUE = 2,
	CHANNEL_ALPHA = 3,
} t3dSourceChannel_t;

// From src/assets/resources/terrain_3d_rendering_material.h
typedef enum t3dSurfaceTextureLayout_e {
	SURFACE_TEXTURE_ORM = 0,
	SURFACE_TEXTURE_ROUGHNESS_R = 1,
	SURFACE_TEXTURE_ROUGHNESS_G = 2,
	SURFACE_TEXTURE_ROUGHNESS_B = 3,
	SURFACE_TEXTURE_ROUGHNESS_A = 4,
} t3dSurfaceTextureLayout_t;

// From src/authoring/editor/terrain_3d_editor.cpp
typedef enum t3dEditSnapshotKind_e {
	EDIT_SNAPSHOT_MAP = 0,
	EDIT_SNAPSHOT_INSTANCES = 1,
} t3dEditSnapshotKind_t;

// From src/authoring/editor/terrain_3d_editor.h
typedef enum t3dTool_e
{
	REGION,
	SCULPT,
	HEIGHT,
	TEXTURE,
	COLOR,
	ROUGHNESS,
	AUTOSHADER,
	HOLES,
	NAVIGATION,
	INSTANCER,
	ANGLE, // Picking angle; replace with a dedicated picking tool.
	SCALE, // used for picking
	LANDFORM,
	TOOL_MAX,
} t3dTool_t;

// From src/authoring/editor/terrain_3d_editor.h
typedef enum t3dOperation_e
{
	ADD,
	SUBTRACT,
	REPLACE,
	AVERAGE,
	GRADIENT,
	OP_MAX,
} t3dOperation_t;

// From src/authoring/editor/terrain_3d_editor.h
typedef enum t3dAverageMode_e
{
	AVG_HEIGHT,
	AVG_BLEND,
	AVG_ROUGHNESS,
} t3dAverageMode_t;

// From src/authoring/erosion/terrain_3d_erosion_config.h
typedef enum t3dBoundaryMode_e {
	BOUNDARY_CLOSED,
	BOUNDARY_OPEN,
} t3dBoundaryMode_t;

// From src/authoring/erosion/terrain_3d_erosion_job.cpp
typedef enum t3dErosionKernel_e : uint8_t {
	WEATHERING,
	THERMAL_FLUX,
	THERMAL_APPLY,
	RAINFALL,
	HYDRAULIC_FLUX,
	HYDRAULIC_WATER,
	HYDRAULIC_CAPACITY,
	DEBRIS_TRANSFER,
	SEDIMENT_ADVECTION,
	WIND_TRANSPORT,
	WIND_GATHER,
	DUNE_FLUX,
	DUNE_APPLY,
	PAVEMENT,
	WAVE_TRANSPORT,
	WAVE_GATHER,
	BEACH_SMOOTH,
	TRAFFIC,
	STRATIGRAPHY,
	EXPOSURE,
	FINAL_STATISTICS,
	MAXIMUM_FLOW,
	MATERIAL_TRANSITIONS
} t3dErosionKernel_t;

// From src/authoring/erosion/terrain_3d_erosion_job.h
typedef enum t3dJobState_e {
	STATE_NEW,
	STATE_READY,
	STATE_RUNNING,
	STATE_COMPLETED,
	STATE_CANCELLED,
	STATE_FAILED,
	STATE_APPLIED,
} t3dJobState_t;

// From src/authoring/erosion/terrain_3d_erosion_job.h
typedef enum t3dOutputMap_e {
	OUTPUT_HEIGHT_DELTA,
	OUTPUT_FLOW_ACCUMULATION,
	OUTPUT_SEDIMENT,
	OUTPUT_COARSE_SEDIMENT,
	OUTPUT_SAND_SEDIMENT,
	OUTPUT_FINE_SEDIMENT,
	OUTPUT_LOOSE_MATERIAL,
	OUTPUT_OXIDATION,
	OUTPUT_WEATHERING,
	OUTPUT_HYDRAULIC_DEPOSITION,
	OUTPUT_WIND_DEPOSITION,
	OUTPUT_WAVE_DEPOSITION,
	OUTPUT_TRAFFIC,
	OUTPUT_ALLUVIAL_DEPOSITION,
	OUTPUT_DEBRIS_FLOW_DEPOSITION,
	OUTPUT_DUNE_DEPOSITION,
	OUTPUT_SALT_WEATHERING,
	OUTPUT_WET_DRY_CYCLES,
	OUTPUT_EXPOSURE_AGE,
	OUTPUT_SNOWPACK,
	OUTPUT_DESERT_PAVEMENT,
	OUTPUT_STRATIGRAPHY,
	OUTPUT_MATERIAL_ID,
	OUTPUT_MAX,
} t3dOutputMap_t;

// From src/authoring/erosion/terrain_3d_erosion_job.h
typedef enum t3dMaterialTransitionKind_e : uint8_t {
	TRANSITION_NONE,
	TRANSITION_EXPOSED,
	TRANSITION_WEATHERED,
	TRANSITION_OXIDIZED,
	TRANSITION_COARSE_DEPOSIT,
	TRANSITION_FINE_DEPOSIT,
	TRANSITION_SALT_WEATHERED,
	TRANSITION_DESERT_PAVEMENT,
	TRANSITION_VARNISHED,
} t3dMaterialTransitionKind_t;

// From src/authoring/erosion/terrain_3d_erosion_job.h
typedef enum t3dStage_e {
	STAGE_IDLE,
	STAGE_SNAPSHOT,
	STAGE_WEATHERING,
	STAGE_HYDRAULIC,
	STAGE_DEBRIS_FLOW,
	STAGE_WIND,
	STAGE_DUNES,
	STAGE_WAVES,
	STAGE_TRAFFIC,
	STAGE_MATERIALS,
	STAGE_COMPLETE,
	STAGE_CANCELLED,
	STAGE_FAILED,
} t3dStage_t;

// From src/authoring/erosion/terrain_3d_erosion_job.h
typedef enum t3dCurveId_e : uint8_t {
	CURVE_MASTER,
	CURVE_RAINFALL,
	CURVE_WEATHERING,
	CURVE_OXIDATION,
	CURVE_WIND,
	CURVE_WAVE,
	CURVE_TRAFFIC,
	CURVE_TEMPERATURE,
	CURVE_SEA_LEVEL,
	CURVE_SNOWMELT,
	CURVE_SALT_WEATHERING,
	CURVE_DEBRIS_FLOW,
	CURVE_COUNT,
} t3dCurveId_t;

// From src/authoring/baking/terrain_3d_landform_baker.cpp
typedef enum t3dHeightPolicy_e {
	FREE,
	RAISE_ONLY,
	LOWER_ONLY,
} t3dHeightPolicy_t;

// From src/authoring/baking/terrain_3d_landform_baker.h
typedef enum t3dMode_e {
	RIDGE = 0,
	VALLEY,
	SCARP,
	BASIN,
	PLATEAU,
	TERRACE,
	ROAD,
	PASS,
	TALUS,
	HYDRAULIC_CHANNEL,
	THERMAL_RELAXATION,
	DUNE,
	SHORELINE,
	MORPHOLOGY,
	OCCLUSION,
	MODE_MAX,
} t3dMode_t;

// From src/authoring/baking/terrain_3d_landform_baker.h
typedef enum t3dMorphologyMode_e {
	MORPH_EXPAND_PEAKS = 0,
	MORPH_FILL_PITS,
	MORPH_RIDGE_SHARPEN,
	MORPH_VALLEY_SHARPEN,
	MORPH_PLATEAU_CLEAN,
	MORPH_MAX,
} t3dMorphologyMode_t;

// From src/authoring/baking/terrain_3d_landform_baker.h
typedef enum t3dGeometryMode_e {
	GEOMETRY_TERRAIN_ONLY = 0,
	GEOMETRY_MODULAR_INSTANCED,
	GEOMETRY_PROCEDURAL_UNIQUE,
	GEOMETRY_MAX,
} t3dGeometryMode_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef enum t3dVTOuterCompression_e : uint32_t {
	T3D_VT_OUTER_COMPRESSION_NONE = 0u,
	T3D_VT_OUTER_COMPRESSION_ZSTD = 1u,
} t3dVTOuterCompression_t;

// From src/authoring/vt/io/terrain_3d_vt_godot_cooker.h
typedef enum t3dVTGodotSourceFlagsV3_e : uint32_t {
	T3D_VT_GODOT_SOURCE_V3_DIRECT_ORM = 1u << 0u,
	T3D_VT_GODOT_SOURCE_V3_HAS_ALPHA = 1u << 1u,
	T3D_VT_GODOT_SOURCE_V3_ALBEDO_RGBA = 1u << 2u,
} t3dVTGodotSourceFlagsV3_t;

// From src/authoring/vt/io/terrain_3d_vt_godot_cooker.h
typedef enum t3dVTEditorCookStageV3_e : uint32_t {
	T3D_VT_EDITOR_COOK_STAGE_IDLE = 0u,
	T3D_VT_EDITOR_COOK_STAGE_EXTRACT = 1u,
	T3D_VT_EDITOR_COOK_STAGE_FILTER = 2u,
	T3D_VT_EDITOR_COOK_STAGE_PAGE = 3u,
	T3D_VT_EDITOR_COOK_STAGE_ENCODE = 4u,
	T3D_VT_EDITOR_COOK_STAGE_PACK = 5u,
	T3D_VT_EDITOR_COOK_STAGE_COMPLETE = 6u,
} t3dVTEditorCookStageV3_t;

// From src/authoring/vt/build/terrain_3d_vt_page_builder.h
typedef enum t3dVTCookWrapMode_e : uint32_t {
	T3D_VT_COOK_WRAP_REPEAT = 0u,
	T3D_VT_COOK_WRAP_CLAMP = 1u,
} t3dVTCookWrapMode_t;

// From src/authoring/vt/build/terrain_3d_vt_page_builder.h
typedef enum t3dVTSurfaceSourceLayout_e : uint32_t {
	T3D_VT_SURFACE_SOURCE_ORM = 0u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_R = 1u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_G = 2u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_B = 3u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_A = 4u,
} t3dVTSurfaceSourceLayout_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef enum t3dWorldMode_e : uint32_t {
	T3D_WORLD_MODE_DISABLED = 0,
	T3D_WORLD_MODE_EXPLICIT,
	T3D_WORLD_MODE_ELIGIBLE_SCENE,
	T3D_WORLD_MODE_FULL_STATIC_SCENE
} t3dWorldMode_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef enum t3dWorldOwnership_e : uint32_t {
	T3D_WORLD_OWNERSHIP_NATIVE_GODOT = 0,
	T3D_WORLD_OWNERSHIP_WORLD,
	T3D_WORLD_OWNERSHIP_COMPATIBILITY,
	T3D_WORLD_OWNERSHIP_EDITOR_PROXY
} t3dWorldOwnership_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef enum t3dWorldCandidateKind_e : uint32_t {
	T3D_WORLD_CANDIDATE_UNKNOWN = 0,
	T3D_WORLD_CANDIDATE_MESH_INSTANCE,
	T3D_WORLD_CANDIDATE_WORLD_INSTANCE,
	T3D_WORLD_CANDIDATE_MULTIMESH_INSTANCE,
	T3D_WORLD_CANDIDATE_SKINNED_MESH
} t3dWorldCandidateKind_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef enum t3dWorldConversionReason_e : uint32_t {
	T3D_WORLD_CONVERSION_OK = 0,
	T3D_WORLD_CONVERSION_DISABLED,
	T3D_WORLD_CONVERSION_NOT_EXPLICIT,
	T3D_WORLD_CONVERSION_EXCLUDED,
	T3D_WORLD_CONVERSION_HIDDEN,
	T3D_WORLD_CONVERSION_DYNAMIC_DISABLED,
	T3D_WORLD_CONVERSION_UNSUPPORTED_KIND,
	T3D_WORLD_CONVERSION_MISSING_MESH,
	T3D_WORLD_CONVERSION_UNSUPPORTED_PRIMITIVE,
	T3D_WORLD_CONVERSION_UNSUPPORTED_MATERIAL,
	T3D_WORLD_CONVERSION_SORTED_TRANSPARENCY,
	T3D_WORLD_CONVERSION_TWO_SIDED_NON_FOLIAGE,
	T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED,
	T3D_WORLD_CONVERSION_COOK_FAILED,
	T3D_WORLD_CONVERSION_DUPLICATE_SOURCE
} t3dWorldConversionReason_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef enum t3dWorldImportCategory_e : uint32_t {
	T3D_WORLD_IMPORT_AUTO = 0,
	T3D_WORLD_IMPORT_WORLD,
	T3D_WORLD_IMPORT_CLUTTER,
	T3D_WORLD_IMPORT_FOLIAGE
} t3dWorldImportCategory_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef enum t3dWorldMaterialPolicy_e : uint32_t {
	T3D_WORLD_MATERIAL_AUTO_CONVERT = 0,
	T3D_WORLD_MATERIAL_REQUIRE_WORLD,
	T3D_WORLD_MATERIAL_NATIVE_ON_UNSUPPORTED
} t3dWorldMaterialPolicy_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef enum t3dWorldTransparencyPolicy_e : uint32_t {
	T3D_WORLD_TRANSPARENCY_NATIVE_SORTED = 0,
	T3D_WORLD_TRANSPARENCY_ALPHA_SCISSOR_ONLY
} t3dWorldTransparencyPolicy_t;

// From src/Terrain3DTypes.h
typedef enum t3dNativeAllocationDomain_e : uint8_t {
	TERRAIN3D_NATIVE_ALLOCATION_PERSISTENT_RESOURCE = 0u,
	TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED,
	TERRAIN3D_NATIVE_ALLOCATION_PARENT_BOUND,
} t3dNativeAllocationDomain_t;

// From src/core/config/terrain_3d_startup_state.h
typedef enum t3dStartupStage_e : uint8_t {
	T3D_STARTUP_IDLE = 0u,
	T3D_STARTUP_REQUESTED,
	T3D_STARTUP_PREPARING,
	T3D_STARTUP_CPU_READY,
	T3D_STARTUP_GPU_PENDING,
	T3D_STARTUP_GPU_READY,
	T3D_STARTUP_COMMITTED,
	T3D_STARTUP_FAILED,
	T3D_STARTUP_CANCELLED,
} t3dStartupStage_t;

// From src/data/model/terrain_3d_data.h
typedef enum t3dHeightFilter_e {
	HEIGHT_FILTER_NEAREST,
	HEIGHT_FILTER_MINIMUM
} t3dHeightFilter_t;

// From src/data/model/terrain_3d_data.h
typedef enum t3dMapEditCommitFlag_e : uint32_t {
	MAP_EDIT_UPDATE_INSTANCES = 1u << 0,
	MAP_EDIT_UPDATE_COLLISION = 1u << 1,
	MAP_EDIT_SNAP = 1u << 2,
	MAP_EDIT_KEEP_EDITED = 1u << 3,
} t3dMapEditCommitFlag_t;

// From src/data/formats/terrain_3d_packed_transform.h
typedef enum t3dInstanceFollowBehavior_e {
	FOLLOW_TERRAIN_HEIGHT = 1u << 0u,
	FOLLOW_TERRAIN_NORMAL = 1u << 1u,
	MANUAL_ORIENTATION = 1u << 2u,
} t3dInstanceFollowBehavior_t;

// From src/data/model/terrain_3d_region.h
typedef enum t3dMapType_e {
	TYPE_HEIGHT,
	TYPE_CONTROL,
	TYPE_COLOR,
	REGION_TYPE_MAX,
} t3dMapType_t;

// From src/data/model/terrain_3d_region.h
typedef enum t3dChannelFlag_e : uint8_t {
	REGION_CHANNEL_CONTROL = 1u << 0,
	REGION_CHANNEL_COLOR = 1u << 1,
	REGION_CHANNEL_DERIVED = 1u << 2,
	REGION_CHANNEL_MASK = REGION_CHANNEL_CONTROL | REGION_CHANNEL_COLOR | REGION_CHANNEL_DERIVED,
} t3dChannelFlag_t;

// From src/data/model/terrain_3d_region.h
typedef enum t3dStreamedNodeBoundaryPolicy_e : int {
	STREAMED_NODE_BOUNDARY_ANCHOR = 0,
	STREAMED_NODE_BOUNDARY_COVERAGE = 1,
} t3dStreamedNodeBoundaryPolicy_t;

// From src/data/model/terrain_3d_region.h
typedef enum t3dEnvironmentLightingClass_e : int {
	LIGHTING_CLASS_TERRAIN = 0,
	LIGHTING_CLASS_FOLIAGE_CLUTTER = 1,
	LIGHTING_CLASS_MEDIUM_OBSTACLE = 2,
	LIGHTING_CLASS_LARGE_OCCLUDER = 3,
	LIGHTING_CLASS_REGIONAL_OCCLUDER = 4,
} t3dEnvironmentLightingClass_t;

// From src/data/model/terrain_3d_region.h
typedef enum t3dBakedLightingMode_e : int {
	BAKED_LIGHTING_DISABLED = 0,
	BAKED_LIGHTING_LIGHTMAP = 1,
	BAKED_LIGHTING_PROBE = 2,
	BAKED_LIGHTING_MATERIAL_ONLY = 3,
} t3dBakedLightingMode_t;

// From src/data/runtime/terrain_3d_runtime_page_format.h
typedef enum t3dRuntimePageChannel_e : uint32_t {
	TERRAIN3D_RUNTIME_PAGE_HEIGHT = 1u,
	TERRAIN3D_RUNTIME_PAGE_CONTROL = 2u,
	TERRAIN3D_RUNTIME_PAGE_COLOR = 3u,
	TERRAIN3D_RUNTIME_PAGE_DERIVED = 4u,
	TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT = 5u,
	TERRAIN3D_RUNTIME_PAGE_FAR_HEIGHT = 6u,
	TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT = 7u,
	TERRAIN3D_RUNTIME_PAGE_FAR_CONTROL = 8u,
	TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE = 9u,
	TERRAIN3D_RUNTIME_PAGE_FAR_ALBEDO_REFERENCE = 10u,
	TERRAIN3D_RUNTIME_PAGE_HOLE_MASK = 11u,
	TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT = 12u,
	TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS = 13u,
	TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES = 14u,
	TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS = 15u,
	TERRAIN3D_RUNTIME_PAGE_HLOD = 16u,
	TERRAIN3D_RUNTIME_PAGE_NAVIGATION = 17u,
	TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA = 18u,
	TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS = 19u,
	TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START = 20u,
	TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK = 21u,
} t3dRuntimePageChannel_t;

// From src/data/runtime/terrain_3d_runtime_page_format.h
typedef enum t3dRuntimePageCompression_e : uint32_t {
	TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE = 0u,
	TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE = 1u,
} t3dRuntimePageCompression_t;

// From src/instances/contracts/terrain_3d_instance_mutation.h
typedef enum t3dInstanceMutationFlags_e : uint32_t {
	T3D_INSTANCE_MUTATION_NONE = 0u,
	T3D_INSTANCE_MUTATION_TRANSFORM = 1u << 0u,
	T3D_INSTANCE_MUTATION_TOPOLOGY = 1u << 1u,
	T3D_INSTANCE_MUTATION_POLICY = 1u << 2u,
	T3D_INSTANCE_MUTATION_REGION_UNLOAD = 1u << 3u,
	T3D_INSTANCE_MUTATION_BULK = 1u << 4u,
} t3dInstanceMutationFlags_t;

// From src/instances/physics/terrain_3d_instance_occlusion.h
typedef enum t3dInstanceOcclusionRecordFlags_e : uint8_t {
	T3D_INSTANCE_OCCLUSION_RECORD_NONE = 0u,
	T3D_INSTANCE_OCCLUSION_RECORD_MIRRORED = 1u << 0u,
} t3dInstanceOcclusionRecordFlags_t;

// From src/instances/storage/terrain_3d_instance_store.h
typedef enum t3dInstanceStoreAllocationClass_e : uint8_t {
	TERRAIN3D_INSTANCE_STORE_PERSISTENT_RESOURCE = 0u,
	TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED,
} t3dInstanceStoreAllocationClass_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef enum t3dInstancerMode_e {
	INSTANCER_DISABLED,
	NORMAL,
} t3dInstancerMode_t;

// From src/lighting/runtime/terrain_3d_lightmap_streamer.h
typedef enum t3dSourceLoadResult_e {
	SOURCE_LOAD_PENDING,
	SOURCE_LOAD_READY,
	SOURCE_LOAD_FAILED,
} t3dSourceLoadResult_t;

// From src/memory/storage/terrain_3d_active_storage.h
typedef enum t3dActiveStorageState_e : uint32_t
	{
		TERRAIN3D_ACTIVE_STORAGE_FREE = 0u,
		TERRAIN3D_ACTIVE_STORAGE_ACTIVE = 1u,
		TERRAIN3D_ACTIVE_STORAGE_DRAINING = 2u,
	} t3dActiveStorageState_t;

// From src/memory/storage/terrain_3d_active_storage.h
typedef enum t3dActivePageState_e : uint32_t
	{
		TERRAIN3D_ACTIVE_PAGE_FREE = 0u,
		TERRAIN3D_ACTIVE_PAGE_STAGING = 1u,
		TERRAIN3D_ACTIVE_PAGE_PUBLISHED = 2u,
		TERRAIN3D_ACTIVE_PAGE_RETIRING = 3u,
	} t3dActivePageState_t;

// From src/memory/core/terrain_3d_memory.h
typedef enum t3dRuntimeAllocationPhase_e : uint8_t {
	TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION = 0,
	TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD,
	TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME,
	TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER,
	TERRAIN3D_ALLOCATION_PHASE_STREAMING_WORKER,
	TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN,
} t3dRuntimeAllocationPhase_t;

// From src/memory/policy/terrain_3d_memory_budget.h
typedef enum t3dMemorySubsystem_e : uint8_t {
	TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES = 0,
	TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES,
	TERRAIN3D_MEMORY_INSTANCE_PAGES,
	TERRAIN3D_MEMORY_BAKED_LIGHTING,
	TERRAIN3D_MEMORY_HLOD_PROXIES,
	TERRAIN3D_MEMORY_SHADER_CACHE,
	TERRAIN3D_MEMORY_SUBSYSTEM_COUNT,
} t3dMemorySubsystem_t;

// From src/memory/policy/terrain_3d_memory_budget.h
typedef enum t3dMemoryLimitReason_e : uint8_t {
	TERRAIN3D_MEMORY_LIMIT_NONE = 0,
	TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET,
	TERRAIN3D_MEMORY_LIMIT_FIXED_CAP,
	TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE,
	TERRAIN3D_MEMORY_LIMIT_DISABLED,
} t3dMemoryLimitReason_t;

// From src/memory/arena/terrain_3d_memory_zone.h
typedef enum t3dMemoryZoneState_e : uint8_t {
	T3D_MEMORY_ZONE_UNINITIALIZED,
	T3D_MEMORY_ZONE_ACTIVE,
	T3D_MEMORY_ZONE_DRAINING,
	T3D_MEMORY_ZONE_DEAD,
} t3dMemoryZoneState_t;

// From src/memory/storage/terrain_3d_resident_pool.h
typedef enum t3dChannel_e : uint32_t {
	RESIDENT_CHANNEL_HEIGHT = 1u << 0u,
	RESIDENT_CHANNEL_CONTROL = 1u << 1u,
	RESIDENT_CHANNEL_COLOR = 1u << 2u,
	RESIDENT_CHANNEL_DERIVED = 1u << 3u,
} t3dChannel_t;

// From src/memory/storage/terrain_3d_resident_pool.h
typedef enum t3dResidentPoolState_e : uint8_t {
	T3D_RESIDENT_POOL_INACTIVE = 0u,
	T3D_RESIDENT_POOL_ACTIVE,
	T3D_RESIDENT_POOL_DRAINING,
} t3dResidentPoolState_t;

// From src/memory/arena/terrain_3d_zone_allocator.h
typedef enum t3dAllocatorAllocationOrigin_e : uint8_t {
	TERRAIN3D_ALLOCATION_BOOTSTRAP = 0,
	TERRAIN3D_ALLOCATION_ZONE,
	TERRAIN3D_ALLOCATION_OVERFLOW,
} t3dAllocatorAllocationOrigin_t;

// From src/memory/arena/terrain_3d_zone_allocator.h
typedef enum t3dAllocatorOverflowPolicy_e : uint8_t {
	TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT = 0,
	TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP,
} t3dAllocatorOverflowPolicy_t;

// From src/physics/runtime/terrain_3d_collision.h
typedef enum t3dCollisionMode_e {
	COLLISION_DISABLED,
	DYNAMIC_GAME,
	DYNAMIC_EDITOR,
	FULL_GAME,
	FULL_EDITOR,
} t3dCollisionMode_t;

// From src/rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h
typedef enum t3dRdPipelineKind_e : uint8_t {
	T3D_RD_PIPELINE_KIND_NONE = 0u,
	T3D_RD_PIPELINE_KIND_RASTER,
	T3D_RD_PIPELINE_KIND_COMPUTE,
} t3dRdPipelineKind_t;

// From src/rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer_types.h
enum Terrain3DGpuBufferPolicy : uint8_t
{
	T3D_GPU_PUBLICATION,
	T3D_GPU_DIRTY_UPLOAD,
	T3D_GPU_HOT_FRAME,
	T3D_GPU_SCRATCH,
	T3D_GPU_HISTORY,
	T3D_GPU_INDIRECT,
	T3D_GPU_NEUTRAL,
	T3D_GPU_POLICY_COUNT,
};

// From src/rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer_types.h
enum Terrain3DGpuGrowthPolicy : uint8_t
{
	T3D_GPU_FIXED,
	T3D_GPU_EXACT,
	T3D_GPU_HIGH_WATER_1_5X,
	T3D_GPU_POWER_OF_TWO,
	T3D_GPU_FRAME_RING,
};

// From src/streaming/runtime/terrain_3d_runtime_page_store.cpp
enum t3dRuntimePageStageFailure_e
{
	T3D_RUNTIME_PAGE_STAGE_OK = 0u,
	T3D_RUNTIME_PAGE_STAGE_HEIGHT = 1u << 0u,
	T3D_RUNTIME_PAGE_STAGE_CONTROL = 1u << 1u,
	T3D_RUNTIME_PAGE_STAGE_COLOR = 1u << 2u,
	T3D_RUNTIME_PAGE_STAGE_DERIVED = 1u << 3u,
	T3D_RUNTIME_PAGE_STAGE_MIDDLE_HEIGHT = 1u << 4u,
	T3D_RUNTIME_PAGE_STAGE_MIDDLE_COMPOSITE = 1u << 5u,
	T3D_RUNTIME_PAGE_STAGE_TERRAIN_SHADOW_MASK = 1u << 6u,
	T3D_RUNTIME_PAGE_STAGE_STATIC_INSTANCES = 1u << 7u,
	T3D_RUNTIME_PAGE_STAGE_HOLE_MASK = 1u << 8u,
	T3D_RUNTIME_PAGE_STAGE_COLLISION_HEIGHT = 1u << 9u,
	T3D_RUNTIME_PAGE_STAGE_COLLISION_BOUNDS = 1u << 10u,
	T3D_RUNTIME_PAGE_STAGE_NAVIGATION = 1u << 11u,
	T3D_RUNTIME_PAGE_STAGE_MIN_MAX_HEIGHT = 1u << 12u,
	T3D_RUNTIME_PAGE_STAGE_NODE_DESCRIPTORS = 1u << 13u,
	T3D_RUNTIME_PAGE_STAGE_INSTANCE_METADATA = 1u << 14u,
	T3D_RUNTIME_PAGE_STAGE_LIGHTING_RECEIVERS = 1u << 15u,
	T3D_RUNTIME_PAGE_STAGE_MULTIMESH_FAST_START = 1u << 16u,
};

// From src/rendering/rd/infrastructure/telemetry/terrain_3d_render_telemetry.h
typedef enum t3dRenderTelemetryEvent_e : uint8_t {
	EVENT_BUFFER_CREATE,
	EVENT_BUFFER_REBUILD,
	EVENT_DESCRIPTOR_CREATE,
	EVENT_PIPELINE_CREATE,
	EVENT_DESCRIPTOR_EVICT,
	EVENT_PIPELINE_EVICT,
	EVENT_TARGET_CREATE,
	EVENT_TARGET_REUSE,
	EVENT_HIZ_BUILD,
	EVENT_HIZ_REBIND,
	EVENT_RESOURCE_FAILURE,
	EVENT_COUNT,
} t3dRenderTelemetryEvent_t;

// From src/rendering/rd/forward/terrain_3d_forward_pass_preparer.h
typedef enum t3dRdForwardLaneMask_e : uint32_t {
		T3D_RD_FORWARD_LANE_TERRAIN = 1u << 0u,
		T3D_RD_FORWARD_LANE_INSTANCES = 1u << 1u,
		T3D_RD_FORWARD_LANE_OCEAN = 1u << 2u,
		T3D_RD_FORWARD_LANE_WORLD = 1u << 4u,
	} t3dRdForwardLaneMask_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
typedef enum t3dRdResourceType_e : uint8_t { 
		T3D_RD_CONTEXT_RESOURCE_UNKNOWN = 0,
		T3D_RD_CONTEXT_RESOURCE_SAMPLER,
		T3D_RD_CONTEXT_RESOURCE_SHADER,
		T3D_RD_CONTEXT_RESOURCE_RASTER_PIPELINE,
		T3D_RD_CONTEXT_RESOURCE_COMPUTE_PIPELINE,
		T3D_RD_CONTEXT_RESOURCE_VERTEX_ARRAY,
		T3D_RD_CONTEXT_RESOURCE_INDEX_ARRAY,
		T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER,
		T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER,
		T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER,
		T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER,
		T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET,
		T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER,
		T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER,
		T3D_RD_CONTEXT_RESOURCE_SHADOW,
		T3D_RD_CONTEXT_RESOURCE_FRAMEBUFFER,
		T3D_RD_CONTEXT_RESOURCE_TEXTURE,
		T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT,
	} t3dRdResourceType_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
typedef enum t3dRdMemoryCategory_e : uint8_t { 
		T3D_RD_CONTEXT_MEMORY_NONE = 0,
		T3D_RD_CONTEXT_MEMORY_TOPOLOGY_VERTEX,
		T3D_RD_CONTEXT_MEMORY_TOPOLOGY_INDEX,
		T3D_RD_CONTEXT_MEMORY_MESH_VERTEX_ARENA,
		T3D_RD_CONTEXT_MEMORY_MESH_INDEX_ARENA,
		T3D_RD_CONTEXT_MEMORY_MATERIAL_TABLE,
		T3D_RD_CONTEXT_MEMORY_INSTANCE,
		T3D_RD_CONTEXT_MEMORY_HLOD,
		T3D_RD_CONTEXT_MEMORY_PATCH,
		T3D_RD_CONTEXT_MEMORY_INDIRECT,
		T3D_RD_CONTEXT_MEMORY_UNIFORM,
		T3D_RD_CONTEXT_MEMORY_STORAGE,
		T3D_RD_CONTEXT_MEMORY_HISTORY,
		T3D_RD_CONTEXT_MEMORY_SCRATCH,
		T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE,
		T3D_RD_CONTEXT_MEMORY_SHADOW,
		T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT,
	} t3dRdMemoryCategory_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
typedef enum t3dRdResourceOwner_e : uint8_t { 
		T3D_RD_CONTEXT_OWNER_UNSPECIFIED = 0,
		T3D_RD_CONTEXT_OWNER_TERRAIN,
		T3D_RD_CONTEXT_OWNER_WORLD,
		T3D_RD_CONTEXT_OWNER_INSTANCES,
		T3D_RD_CONTEXT_OWNER_HLOD,
		T3D_RD_CONTEXT_OWNER_OCEAN,
		T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING,
		T3D_RD_CONTEXT_OWNER_VT,
		T3D_RD_CONTEXT_OWNER_SHADER_CACHE,
		T3D_RD_CONTEXT_OWNER_COUNT,
	} t3dRdResourceOwner_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
typedef enum t3dRdFixedIndirectSlot_e : uint8_t { 
		T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD,
		T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN,
		T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP,
		T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT,
	} t3dRdFixedIndirectSlot_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
typedef enum t3dRdContextState_e : uint8_t { 
		T3D_RD_CONTEXT_STATE_UNINITIALIZED = 0,
		T3D_RD_CONTEXT_STATE_READY,
		T3D_RD_CONTEXT_STATE_FAILED,
		T3D_RD_CONTEXT_STATE_REBUILD_REQUIRED,
		T3D_RD_CONTEXT_STATE_REBUILDING,
		T3D_RD_CONTEXT_STATE_SHUTTING_DOWN,
	} t3dRdContextState_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
typedef enum t3dRdFailureCode_e : uint8_t { 
		T3D_RD_CONTEXT_FAILURE_NONE = 0,
		T3D_RD_CONTEXT_FAILURE_DEVICE_UNAVAILABLE,
		T3D_RD_CONTEXT_FAILURE_RESOURCE_CREATION,
		T3D_RD_CONTEXT_FAILURE_DUPLICATE_RESOURCE,
		T3D_RD_CONTEXT_FAILURE_UNTRACKED_RESOURCE,
		T3D_RD_CONTEXT_FAILURE_ACCOUNTING,
		T3D_RD_CONTEXT_FAILURE_DEVICE_INVALIDATED,
		T3D_RD_CONTEXT_FAILURE_REBUILD,
		T3D_RD_CONTEXT_FAILURE_RENDER_PASS,
	} t3dRdFailureCode_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
typedef enum t3dRdSuccessfulPass_e : uint8_t { 
		T3D_RD_CONTEXT_PASS_NONE = 0,
		T3D_RD_CONTEXT_PASS_TERRAIN_CLIPMAP,
		T3D_RD_CONTEXT_PASS_COUNT,
	} t3dRdSuccessfulPass_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef enum t3dRdForwardBridgeCapability_e : uint64_t {
	T3D_RD_FORWARD_CAP_DIRECT_POD = uint64_t( 1u ) << 0u,
	T3D_RD_FORWARD_CAP_DEPTH = uint64_t( 1u ) << 1u,
	T3D_RD_FORWARD_CAP_OPAQUE = uint64_t( 1u ) << 2u,
	T3D_RD_FORWARD_CAP_SHADOW = uint64_t( 1u ) << 3u,
	T3D_RD_FORWARD_CAP_MOTION = uint64_t( 1u ) << 4u,
	T3D_RD_FORWARD_CAP_TRANSPARENT = uint64_t( 1u ) << 5u,
	T3D_RD_FORWARD_CAP_CLUSTERED_LIGHTING = uint64_t( 1u ) << 6u,
	T3D_RD_FORWARD_CAP_DECALS = uint64_t( 1u ) << 7u,
	T3D_RD_FORWARD_CAP_REFLECTIONS = uint64_t( 1u ) << 8u,
	T3D_RD_FORWARD_CAP_RESOURCE_GENERATIONS = uint64_t( 1u ) << 9u,
	T3D_RD_FORWARD_CAP_SPLIT_SHADOW_PACKET = uint64_t( 1u ) << 10u,
	T3D_RD_FORWARD_CAP_ENVIRONMENT_LIGHTING = uint64_t( 1u ) << 11u,
	T3D_RD_FORWARD_CAP_PREVIOUS_DEPTH_HIZ = uint64_t( 1u ) << 12u,
} t3dRdForwardBridgeCapability_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef enum t3dRdForwardBridgeEnvironmentFlag_e : uint32_t {
	T3D_RD_FORWARD_BRIDGE_ENV_USE_AMBIENT_LIGHT = 1u << 0u,
	T3D_RD_FORWARD_BRIDGE_ENV_USE_AMBIENT_CUBEMAP = 1u << 1u,
	T3D_RD_FORWARD_BRIDGE_ENV_USE_REFLECTION_CUBEMAP = 1u << 2u,
	T3D_RD_FORWARD_BRIDGE_ENV_RADIANCE_OCTMAP_ARRAY = 1u << 3u,
} t3dRdForwardBridgeEnvironmentFlag_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef enum t3dRdForwardBridgeFrameFlag_e : uint32_t {
	T3D_RD_FORWARD_FRAME_MOTION_VECTORS = 1u << 0u,
	T3D_RD_FORWARD_FRAME_DIRECTIONAL_SOFT_SHADOWS = 1u << 1u,
} t3dRdForwardBridgeFrameFlag_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef enum t3dRdForwardBridgePassFlag_e : uint32_t {
	T3D_RD_FORWARD_PASS_DEPTH_PREPASS_ACTIVE = 1u << 0u,
	T3D_RD_FORWARD_PASS_REVERSE_CULL = 1u << 1u,
} t3dRdForwardBridgePassFlag_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_variant.h
typedef enum t3dRdForwardVariantCapability_e : uint32_t {
	T3D_RD_FORWARD_VARIANT_ALLOW_TRANSPARENT = 1u << 0u,
	T3D_RD_FORWARD_VARIANT_SHADOW_ANY_NATIVE_MODE = 1u << 1u,
} t3dRdForwardVariantCapability_t;

// From src/rendering/rd/instances/terrain_3d_rd_mesh_cache.h
typedef enum t3dRdExternalMeshSourceFlags_e : uint32_t {
	T3D_RD_EXTERNAL_MESH_REQUIRE_RENDERING_MATERIAL = 1u << 0u,
} t3dRdExternalMeshSourceFlags_t;

// From src/Terrain3DTypes.h
typedef enum t3dRdPreviewFailure_e {
	T3D_RD_PREVIEW_NONE,
	T3D_RD_PREVIEW_ASSET_SOURCE,
	T3D_RD_PREVIEW_TRANSIENT_DEFERRED,
	T3D_RD_PREVIEW_SYSTEMIC_RD,
} t3dRdPreviewFailure_t;

// From src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.h
typedef enum t3dRdShaderDomain_e : uint8_t {
	T3D_RD_SHADER_GLOBAL_DEVICE,
	T3D_RD_SHADER_LOCAL_DEVICE,
} t3dRdShaderDomain_t;

// From src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.h
typedef enum t3dRdShaderCategory_e : uint8_t {
	T3D_RD_SHADER_CATEGORY_TERRAIN,
	T3D_RD_SHADER_CATEGORY_INSTANCE,
	T3D_RD_SHADER_CATEGORY_WORLD,
	T3D_RD_SHADER_CATEGORY_OCEAN,
	T3D_RD_SHADER_CATEGORY_COMPUTE,
} t3dRdShaderCategory_t;

// From src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp
typedef enum t3dRdShaderSourceStage_e : uint8_t {
	T3D_RD_SHADER_SOURCE_IDLE = 0u,
	T3D_RD_SHADER_SOURCE_REQUESTED,
	T3D_RD_SHADER_SOURCE_READY,
	T3D_RD_SHADER_SOURCE_FAILED,
} t3dRdShaderSourceStage_t;

// From src/rendering/rd/resources/terrain_3d_rd_texture_manager.h
typedef enum t3dRdTextureSemantic_e : uint8_t {
	T3D_RD_TEXTURE_LINEAR_DATA = 0,
	T3D_RD_TEXTURE_SOURCE_COLOR = 1,
} t3dRdTextureSemantic_t;

// From src/rendering/rd/resources/terrain_3d_rd_texture_manager.h
typedef enum t3dRdTextureBatchState_e : uint8_t {
	T3D_RD_TEXTURE_BATCH_UNUSED = 0u,
	T3D_RD_TEXTURE_BATCH_PENDING = 1u,
	T3D_RD_TEXTURE_BATCH_SUCCEEDED = 2u,
	T3D_RD_TEXTURE_BATCH_FAILED = 3u,
	T3D_RD_TEXTURE_BATCH_WRITING = 4u,
} t3dRdTextureBatchState_t;

// From src/rendering/rd/resources/terrain_3d_rd_texture_manager.h
typedef enum t3dRdTextureAsyncKind_e : uint8_t {
		T3D_RD_TEXTURE_ASYNC_NONE = 0u,
		T3D_RD_TEXTURE_ASYNC_UPDATE = 1u,
		T3D_RD_TEXTURE_ASYNC_REGION_UPDATE = 2u,
	} t3dRdTextureAsyncKind_t;

// From src/rendering/rd/resources/terrain_3d_rd_texture_manager.h
typedef enum t3dRdTextureAsyncState_e : uint8_t {
		T3D_RD_TEXTURE_ASYNC_UNUSED = 0u,
		T3D_RD_TEXTURE_ASYNC_WRITING = 1u,
		T3D_RD_TEXTURE_ASYNC_PENDING = 2u,
	} t3dRdTextureAsyncState_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef enum t3dRdForwardCallbackPhase_e {
	T3D_RD_FORWARD_PHASE_PREPARE = 0,
	T3D_RD_FORWARD_PHASE_SUBMIT = 1,
} t3dRdForwardCallbackPhase_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef enum t3dRdForwardEnvironmentFlag_e : uint32_t {
	T3D_RD_FORWARD_ENV_USE_AMBIENT_LIGHT = 1u << 0u,
	T3D_RD_FORWARD_ENV_USE_AMBIENT_CUBEMAP = 1u << 1u,
	T3D_RD_FORWARD_ENV_USE_REFLECTION_CUBEMAP = 1u << 2u,
	T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY = 1u << 3u,
} t3dRdForwardEnvironmentFlag_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef enum t3dRdDrawGroup_e {
	T3D_RD_DRAW_GROUP_LOD0_NEAR = 0,
	T3D_RD_DRAW_GROUP_OUTER_NEAR = 1,
	T3D_RD_DRAW_GROUP_OUTER_MIDDLE = 2,
	T3D_RD_DRAW_GROUP_OUTER_FAR = 3,
} t3dRdDrawGroup_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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
	T3D_RD_MATERIAL_FEATURE_MICRO_VT = 1u << 14u,
	T3D_RD_MATERIAL_FEATURE_MEGAVT = 1u << 15u,
	T3D_RD_MATERIAL_FEATURE_FLAT_TERRAIN_NORMALS = 1u << 16u,
} t3dRdMaterialFeatureBits_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef enum t3dRdMaterialOutputBits_e {
	T3D_RD_MATERIAL_OUTPUT_ALBEDO = 1u << 0u,
	T3D_RD_MATERIAL_OUTPUT_ROUGHNESS = 1u << 1u,
	T3D_RD_MATERIAL_OUTPUT_SPECULAR = 1u << 2u,
	T3D_RD_MATERIAL_OUTPUT_NORMAL = 1u << 3u,
	T3D_RD_MATERIAL_OUTPUT_AO = 1u << 4u,
} t3dRdMaterialOutputBits_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef enum t3dRdMaterialLayerBits_e {
	T3D_RD_MATERIAL_LAYER_PACKED_SURFACE = 1u << 0u,
	T3D_RD_MATERIAL_LAYER_RESIDENT = 1u << 1u,
	T3D_RD_MATERIAL_LAYER_MICRO_VT = 1u << 2u,
	T3D_RD_MATERIAL_LAYER_CANONICAL_VT_V3 = 1u << 3u,
} t3dRdMaterialLayerBits_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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
	T3D_RD_INSTANCE_CATEGORY_SHIFT = 26u,
	T3D_RD_INSTANCE_CATEGORY_MASK = 3u << T3D_RD_INSTANCE_CATEGORY_SHIFT,
	T3D_RD_INSTANCE_CATEGORY_CLUTTER = 0u << T3D_RD_INSTANCE_CATEGORY_SHIFT,
	T3D_RD_INSTANCE_CATEGORY_GROUND_COVER = 1u << T3D_RD_INSTANCE_CATEGORY_SHIFT,
	T3D_RD_INSTANCE_CATEGORY_OCCLUDER = 2u << T3D_RD_INSTANCE_CATEGORY_SHIFT,
	T3D_RD_INSTANCE_LOD_PRESELECTED = 1u << 28u,
	T3D_RD_INSTANCE_FADE_SHIFT = 8u,
	T3D_RD_INSTANCE_FADE_MASK = 0x00FFFF00u,
	T3D_RD_INSTANCE_DEPTH_PREPASS_PASS = 1u << 31u,
} t3dRdInstanceFlags_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_render_coordinator.h
typedef enum t3dStartupGpuStage_e : uint32_t {
		T3D_STARTUP_GPU_VT_STORAGE = 0u,
		T3D_STARTUP_GPU_VT_EXECUTABLE,
		T3D_STARTUP_GPU_VT_PUBLICATION,
		T3D_STARTUP_GPU_SHADER_PREWARM,
		T3D_STARTUP_GPU_TERRAIN_RENDERER,
		T3D_STARTUP_GPU_WORLD_RUNTIME,
		T3D_STARTUP_GPU_INSTANCE_RENDERER,
		T3D_STARTUP_GPU_OCEAN_RENDERER,
		T3D_STARTUP_GPU_COMPLETE,
		T3D_STARTUP_GPU_FAILED,
	} t3dStartupGpuStage_t;

// From src/rendering/rd/core/terrain_3d_render_lifecycle_state.h
typedef enum t3dRenderLifecycleStage_e : uint32_t {
	T3D_RENDER_LIFECYCLE_IDLE = 0u,
	T3D_RENDER_LIFECYCLE_STARTING,
	T3D_RENDER_LIFECYCLE_RECOVERING,
	T3D_RENDER_LIFECYCLE_READY,
	T3D_RENDER_LIFECYCLE_CANCELLING,
	T3D_RENDER_LIFECYCLE_STOPPED,
	T3D_RENDER_LIFECYCLE_FAILED,
} t3dRenderLifecycleStage_t;

// From src/rendering/rd/core/terrain_3d_render_lifecycle_state.h
typedef enum t3dRenderReadyBits_e : uint32_t {
	T3D_RENDER_READY_NONE = 0u,
	T3D_RENDER_READY_SHADER_GPU = 1u << 0u,
	T3D_RENDER_READY_VT_STORAGE = 1u << 1u,
	T3D_RENDER_READY_VT_EXECUTABLE = 1u << 2u,
	T3D_RENDER_READY_VT_PUBLISHED = 1u << 3u,
	T3D_RENDER_READY_TERRAIN = 1u << 4u,
	T3D_RENDER_READY_WORLD = 1u << 5u,
	T3D_RENDER_READY_INSTANCES = 1u << 6u,
	T3D_RENDER_READY_OCEAN = 1u << 7u,
	T3D_RENDER_READY_PREVIEW = 1u << 8u,
} t3dRenderReadyBits_t;

// From src/rendering/rd/core/terrain_3d_render_lifecycle_state.h
typedef enum t3dRenderLifecycleFailure_e : uint32_t {
	T3D_RENDER_LIFECYCLE_FAILURE_NONE = 0u,
	T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_GPU,
	T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_REQUEUE,
	T3D_RENDER_LIFECYCLE_FAILURE_WORLD_PUBLICATION,
	T3D_RENDER_LIFECYCLE_FAILURE_RUNTIME_RENDER,
	T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_SCHEDULING,
	T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_INITIALIZATION,
	T3D_RENDER_LIFECYCLE_FAILURE_CANCELLATION_SCHEDULING,
	T3D_RENDER_LIFECYCLE_FAILURE_TEARDOWN_SCHEDULING,
} t3dRenderLifecycleFailure_t;

// From src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h
typedef enum t3dVTSharedResourceClass_e {
	T3D_VT_SHARED_RESOURCES_NONE = 0,
	T3D_VT_SHARED_RESOURCES_PREVIEW,
	T3D_VT_SHARED_RESOURCES_PRODUCTION,
} t3dVTSharedResourceClass_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef enum t3dTerrainMegaVTRegionFlags_e : uint8_t
{
	T3D_TERRAIN_MEGAVT_REGION_PRESENT = 1u << 0u,
	T3D_TERRAIN_MEGAVT_REGION_COOKED = 1u << 1u,
	T3D_TERRAIN_MEGAVT_REGION_EDITOR_DYNAMIC = 1u << 2u,
} t3dTerrainMegaVTRegionFlags_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef enum t3dTerrainMegaVTCookFlags_e : uint16_t
{
	T3D_TERRAIN_MEGAVT_COOK_ALL_CHANNELS = ( 1u << T3D_VT_CHANNEL_COUNT ) - 1u,
	T3D_TERRAIN_MEGAVT_COOK_NEIGHBOR_GUTTERS = 1u << 8u,
} t3dTerrainMegaVTCookFlags_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef enum t3dTerrainMegaVTIncrementalFlags_e : uint8_t
{
	T3D_TERRAIN_MEGAVT_INCREMENTAL_EDITOR = 1u << 0u,
	T3D_TERRAIN_MEGAVT_INCREMENTAL_ANCESTORS = 1u << 1u,
	T3D_TERRAIN_MEGAVT_INCREMENTAL_GUTTER_NEIGHBORS = 1u << 2u,
} t3dTerrainMegaVTIncrementalFlags_t;

// From src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.h
typedef enum t3dVTRequestProducer_e : uint16_t {
	T3D_VT_REQUEST_PRODUCER_TERRAIN = 0u,
	T3D_VT_REQUEST_PRODUCER_INSTANCE = 1u,
	T3D_VT_REQUEST_PRODUCER_COUNT = 2u,
} t3dVTRequestProducer_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTChannel_e : uint32_t {
	T3D_VT_CHANNEL_ALBEDO = 0u,
	T3D_VT_CHANNEL_NORMAL = 1u,
	T3D_VT_CHANNEL_SURFACE = 2u,
	T3D_VT_CHANNEL_HEIGHT = 3u,
} t3dVTChannel_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTChannelFormat_e : uint32_t {
	T3D_VT_FORMAT_ALBEDO_BC1_SRGB = 0u,
	T3D_VT_FORMAT_NORMAL_BC5_UNORM = 1u,
	T3D_VT_FORMAT_SURFACE_BC7_UNORM = 2u,
	T3D_VT_FORMAT_HEIGHT_R8_UNORM = 3u,
} t3dVTChannelFormat_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTLogicalChannel_e : uint32_t {
	T3D_VT_LOGICAL_CHANNEL_ALBEDO = 0u,
	T3D_VT_LOGICAL_CHANNEL_PBR = 1u,
	T3D_VT_LOGICAL_CHANNEL_HEIGHT = 2u,
	T3D_VT_LOGICAL_CHANNEL_NORMAL = 3u,
	T3D_VT_LOGICAL_CHANNEL_ORM = 4u,
} t3dVTLogicalChannel_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTLogicalChannelFormat_e : uint32_t {
	T3D_VT_LOGICAL_FORMAT_ALBEDO_BC1_SRGB = 0u,
	T3D_VT_LOGICAL_FORMAT_PBR_BC7_UNORM = 1u,
	T3D_VT_LOGICAL_FORMAT_HEIGHT_R8_UNORM = 2u,
	T3D_VT_LOGICAL_FORMAT_NORMAL_BC5_UNORM = 3u,
	T3D_VT_LOGICAL_FORMAT_ORM_BC1_UNORM = 4u,
} t3dVTLogicalChannelFormat_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTPhysicalFormatFamily_e : uint32_t {
	T3D_VT_PHYSICAL_FORMAT_BC1_UNORM = 0u,
	T3D_VT_PHYSICAL_FORMAT_BC7_UNORM = 1u,
	T3D_VT_PHYSICAL_FORMAT_BC5_UNORM = 2u,
	T3D_VT_PHYSICAL_FORMAT_R8_UNORM = 3u,
	T3D_VT_PHYSICAL_FORMAT_COUNT = 4u,
} t3dVTPhysicalFormatFamily_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTChannelSemantic_e : uint32_t {
	T3D_VT_CHANNEL_SEMANTIC_SRGB_ALBEDO = 0u,
	T3D_VT_CHANNEL_SEMANTIC_TERRAIN_PBR = 1u,
	T3D_VT_CHANNEL_SEMANTIC_LINEAR_HEIGHT = 2u,
	T3D_VT_CHANNEL_SEMANTIC_NORMAL_XY = 3u,
	T3D_VT_CHANNEL_SEMANTIC_LINEAR_ORM = 4u,
} t3dVTChannelSemantic_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTConsumer_e : uint32_t {
	T3D_VT_CONSUMER_CLIPMAP = 0u,
	T3D_VT_CONSUMER_INSTANCE = 1u,
} t3dVTConsumer_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTSamplerClass_e : uint32_t {
	T3D_VT_SAMPLER_LINEAR_REPEAT = 0u,
	T3D_VT_SAMPLER_LINEAR_CLAMP = 1u,
	T3D_VT_SAMPLER_NEAREST_REPEAT = 2u,
	T3D_VT_SAMPLER_NEAREST_CLAMP = 3u,
} t3dVTSamplerClass_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTTextureFilter_e : uint32_t {
	T3D_VT_FILTER_NEAREST = 0u,
	T3D_VT_FILTER_LINEAR = 1u,
	T3D_VT_FILTER_NEAREST_MIPMAP = 2u,
	T3D_VT_FILTER_LINEAR_MIPMAP = 3u,
	T3D_VT_FILTER_NEAREST_MIPMAP_ANISOTROPIC = 4u,
	T3D_VT_FILTER_LINEAR_MIPMAP_ANISOTROPIC = 5u,
} t3dVTTextureFilter_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTAllocationFlags_e : uint16_t {
	T3D_VT_ALLOCATION_MATERIAL_UV = 1u << 0u,
	T3D_VT_ALLOCATION_MEGATEXTURE = 1u << 1u,
	T3D_VT_ALLOCATION_REPEAT = 1u << 2u,
	T3D_VT_ALLOCATION_CLAMP = 1u << 3u,
	T3D_VT_ALLOCATION_HAS_ALPHA = 1u << 4u,
	T3D_VT_ALLOCATION_HAS_HEIGHT = 1u << 5u,
	T3D_VT_ALLOCATION_PINNED = 1u << 6u,
	T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE = 1u << 7u,
	T3D_VT_ALLOCATION_EDITOR_DYNAMIC = 1u << 8u,
	// VT-P07 runtime-only view selection. Serialized authoring flags keep these bits clear.
	T3D_VT_ALLOCATION_QUALITY_BASE_MIP_SHIFT = 9u,
	T3D_VT_ALLOCATION_QUALITY_BASE_MIP_MASK = 0x0fu << T3D_VT_ALLOCATION_QUALITY_BASE_MIP_SHIFT,
	T3D_VT_ALLOCATION_QUALITY_DISABLED = 1u << 13u,
	T3D_VT_ALLOCATION_ALBEDO_RGBA = 1u << 14u,
} t3dVTAllocationFlags_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTPageFlags_e : uint16_t {
	T3D_VT_PAGE_VALID = 1u << 0u,
	T3D_VT_PAGE_RESIDENT = 1u << 1u,
	T3D_VT_PAGE_PINNED = 1u << 2u,
	T3D_VT_PAGE_NEUTRAL = 1u << 3u,
	T3D_VT_PAGE_ROOT = 1u << 4u,
	T3D_VT_PAGE_TAIL_FALLBACK = 1u << 5u,
} t3dVTPageFlags_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTResolveFlags_e : uint16_t {
	T3D_VT_RESOLVE_PHYSICAL = 1u << 0u,
	T3D_VT_RESOLVE_ANCESTOR = 1u << 1u,
	T3D_VT_RESOLVE_TAIL = 1u << 2u,
} t3dVTResolveFlags_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dVTPhysicalSlotFlags_e : uint16_t {
	T3D_VT_SLOT_PINNED = 1u << 0u,
	T3D_VT_SLOT_PENDING = 1u << 1u,
	T3D_VT_SLOT_REFERENCED = 1u << 2u,
	T3D_VT_SLOT_HOT = 1u << 3u,
	T3D_VT_SLOT_RETIRED = 1u << 4u,
} t3dVTPhysicalSlotFlags_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef enum t3dRenderingMaterialFlags_e : uint32_t {
	T3D_VT_MATERIAL_ALPHA_SCISSOR = 1u << 0u,
	T3D_VT_MATERIAL_FOLIAGE = 1u << 1u,
	T3D_VT_MATERIAL_CAST_SHADOWS = 1u << 2u,
	T3D_VT_MATERIAL_RECEIVE_SHADOWS = 1u << 3u,
	T3D_VT_MATERIAL_TWO_SIDED = 1u << 4u,
	T3D_VT_MATERIAL_UNSHADED = 1u << 5u,
	T3D_VT_MATERIAL_VERTEX_COLOR = 1u << 6u,
	T3D_VT_MATERIAL_VERTEX_COLOR_SRGB = 1u << 7u,
	T3D_VT_MATERIAL_NORMAL_MAPPING = 1u << 8u,
	T3D_VT_MATERIAL_AO_ENABLED = 1u << 9u,
	T3D_VT_MATERIAL_HAS_HEIGHT = 1u << 10u,
	T3D_VT_MATERIAL_HAS_ALPHA = 1u << 11u,
	// Imported custom ShaderMaterial profile: ORM channels are authored as
	// absolute AO/roughness/metallic values rather than scalar multipliers.
	T3D_VT_MATERIAL_DIRECT_ORM = 1u << 12u,
	// m_nReserved low 16 bits hold albedo saturation in [0, 1.5].
	T3D_VT_MATERIAL_ALBEDO_SATURATION = 1u << 13u,
	// Known shared ShaderMaterial profile uses the authored stylized sun loop.
	T3D_VT_MATERIAL_STYLIZED_SUN = 1u << 14u,
	// VT-P06: material allocation is the base of a sparse V3 runtime channel triplet.
	T3D_VT_MATERIAL_V3_LAYOUT = 1u << 15u,
} t3dRenderingMaterialFlags_t;

// From src/rendering/rd/vt/runtime/terrain_3d_vt_fixed_profile.h
typedef enum t3dVTFixedProfile_e : uint32_t {
	T3D_VT_FIXED_PROFILE_JUST_MAKE_IT_RUN = 0u,
	T3D_VT_FIXED_PROFILE_LOW = 1u,
	T3D_VT_FIXED_PROFILE_NORMAL = 2u,
	T3D_VT_FIXED_PROFILE_HIGH = 3u,
	T3D_VT_FIXED_PROFILE_VERY_HIGH = 4u,
	T3D_VT_FIXED_PROFILE_MAKE_MY_GPU_SCREAM = 5u,
	T3D_VT_FIXED_PROFILE_COUNT = T3D_VT_QUALITY_PROFILE_COUNT,
	T3D_VT_FIXED_PROFILE_INVALID = 0xffffffffu,
} t3dVTFixedProfile_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef enum t3dVTResidencyCategory_e : uint16_t {
	T3D_VT_RESIDENCY_TERRAIN = 0u,
	T3D_VT_RESIDENCY_CLUTTER = 1u,
	T3D_VT_RESIDENCY_GROUND_COVER = 2u,
	T3D_VT_RESIDENCY_OCCLUDER = 3u,
	T3D_VT_RESIDENCY_HLOD = 4u,
} t3dVTResidencyCategory_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef enum t3dVTResidencyTemperature_e : uint16_t {
	T3D_VT_RESIDENCY_HOT = 0u,
	T3D_VT_RESIDENCY_WARM = 1u,
	T3D_VT_RESIDENCY_COLD = 2u,
} t3dVTResidencyTemperature_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef enum t3dVTRuntimeDatabaseKind_e : uint32_t {
	T3D_VT_RUNTIME_DATABASE_LEGACY_NEUTRAL = 0u,
	T3D_VT_RUNTIME_DATABASE_PACK_V3 = 2u,
} t3dVTRuntimeDatabaseKind_t;

// From src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h
typedef enum t3dVTGpuBufferId_e : uint32_t {
	T3D_VT_GPU_BUFFER_ALLOCATION_TABLE = 0,
	T3D_VT_GPU_BUFFER_PAGE_TABLE,
	T3D_VT_GPU_BUFFER_MIP_LAYOUT_TABLE,
	T3D_VT_GPU_BUFFER_TAIL_METADATA,
	T3D_VT_GPU_BUFFER_FRAGMENT_HASH,
	T3D_VT_GPU_BUFFER_FRAGMENT_COUNTERS,
	T3D_VT_GPU_BUFFER_FRAGMENT_PARAMS,
	T3D_VT_GPU_BUFFER_COUNT
} t3dVTGpuBufferId_t;

// From src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h
typedef enum t3dVTResourceChannel_e : uint32_t {
	T3D_VT_RESOURCE_ALBEDO = 0,
	T3D_VT_RESOURCE_NORMAL,
	T3D_VT_RESOURCE_SURFACE,
	T3D_VT_RESOURCE_HEIGHT
} t3dVTResourceChannel_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef enum t3dVTPriorityBucket_e : uint16_t {
	T3D_VT_PRIORITY_CRITICAL = 0u,
	T3D_VT_PRIORITY_NEAR = 1u,
	T3D_VT_PRIORITY_NORMAL = 2u,
	T3D_VT_PRIORITY_PREFETCH = 3u,
	T3D_VT_PRIORITY_TRANSITION = 4u,
	T3D_VT_PRIORITY_BACKGROUND = 5u
} t3dVTPriorityBucket_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef enum t3dVTAdmissionStatus_e : uint32_t {
	T3D_VT_ADMISSION_ACCEPTED_NEW = 0u,
	T3D_VT_ADMISSION_MERGED_EXISTING = 1u,
	T3D_VT_ADMISSION_ALREADY_RESIDENT = 2u,
	T3D_VT_ADMISSION_DEFERRED_FRAME_BUDGET = 3u,
	T3D_VT_ADMISSION_DROPPED_QUEUE_FULL = 4u,
	T3D_VT_ADMISSION_INVALID_PAGE = 5u,
	T3D_VT_ADMISSION_INVALID_PAYLOAD = 6u,
	T3D_VT_ADMISSION_STATUS_COUNT = 7u
} t3dVTAdmissionStatus_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef enum t3dVTReadbackConsumeStatus_e : uint32_t {
	T3D_VT_READBACK_CONSUMED = 0u,
	T3D_VT_READBACK_STALE_DATABASE = 1u,
	T3D_VT_READBACK_INVALID = 2u
} t3dVTReadbackConsumeStatus_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef enum t3dVTStreamJobState_e : uint16_t {
	T3D_VT_STREAM_FREE = 0,
	T3D_VT_STREAM_QUEUED,
	T3D_VT_STREAM_WORKING,
	T3D_VT_STREAM_READY,
	T3D_VT_STREAM_FAILED,
	T3D_VT_STREAM_STAGED
} t3dVTStreamJobState_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef enum t3dVTStreamResultCode_e : uint32_t {
	T3D_VT_STREAM_RESULT_OK = 0u,
	T3D_VT_STREAM_RESULT_READ_FAILED = 1u,
	T3D_VT_STREAM_RESULT_INVALID_PAYLOAD = 2u,
	T3D_VT_STREAM_RESULT_STALE_DATABASE = 3u,
	T3D_VT_STREAM_RESULT_STALE_PAGE = 4u,
	T3D_VT_STREAM_RESULT_STALE_ALLOCATION = 5u,
	T3D_VT_STREAM_RESULT_OUTPUT_SIZE_MISMATCH = 6u
} t3dVTStreamResultCode_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef enum t3dVTStreamCompletionState_e : uint32_t {
	T3D_VT_STREAM_COMPLETION_IDLE = 0u,
	T3D_VT_STREAM_COMPLETION_READY = 1u,
	T3D_VT_STREAM_COMPLETION_FAILED = 2u
} t3dVTStreamCompletionState_t;

// From src/rendering/rd/world/build/terrain_3d_world_cluster_cooker.h
typedef enum t3dWorldClusterCookFlags_e : uint32_t {
	T3D_WORLD_CLUSTER_COOK_NONE = 0u,
	T3D_WORLD_CLUSTER_COOK_TWO_SIDED = 1u << 0u,
} t3dWorldClusterCookFlags_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef enum t3dWorldGeometryKind_e : uint32_t {
	T3D_WORLD_GEOMETRY_TERRAIN = 0u,
	T3D_WORLD_GEOMETRY_MESH_CLUSTER = 1u,
	T3D_WORLD_GEOMETRY_HLOD_CLUSTER = 2u,
	T3D_WORLD_GEOMETRY_STATIC_CLUSTER = 3u,
	T3D_WORLD_GEOMETRY_KIND_COUNT = 4u,
} t3dWorldGeometryKind_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef enum t3dWorldTier_e : uint32_t {
	T3D_WORLD_TIER_WORLD = 0u,
	T3D_WORLD_TIER_CLUTTER = 1u,
	T3D_WORLD_TIER_FOLIAGE = 2u,
	T3D_WORLD_TIER_COUNT = 3u,
} t3dWorldTier_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef enum t3dWorldGeometryCookResult_e : uint32_t {
	T3D_WORLD_GEOMETRY_COOK_OK = 0,
	T3D_WORLD_GEOMETRY_COOK_INVALID_INPUT,
	T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED,
	T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW,
	T3D_WORLD_GEOMETRY_COOK_INTERNAL_ERROR
} t3dWorldGeometryCookResult_t;

// From src/rendering/rd/world/visibility/terrain_3d_world_raster.h
typedef enum t3dPrimitiveClass_e : uint32_t {
		T3D_PRIMITIVE_TERRAIN_STRIP = 0u,
		T3D_PRIMITIVE_MESH = 1u,
		T3D_PRIMITIVE_CLASS_COUNT = 2u,
	} t3dPrimitiveClass_t;

// From src/rendering/rd/world/resources/terrain_3d_world_resources.h
typedef enum t3dWorldBufferId_e : uint32_t {
		WORLD_BUFFER_VERTICES = 0u,
		WORLD_BUFFER_CLUSTER_VERTEX_INDICES,
		WORLD_BUFFER_TRIANGLES,
		WORLD_BUFFER_CLUSTERS,
		WORLD_BUFFER_CLUSTER_VT_METADATA,
		WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS,
		WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS,
		WORLD_BUFFER_TRANSFORMS_CURRENT,
		WORLD_BUFFER_TRANSFORMS_PREVIOUS,
		WORLD_BUFFER_INSTANCES,
		WORLD_BUFFER_CELLS,
		WORLD_BUFFER_HLOD_RECORDS,
		WORLD_BUFFER_MATERIALS,
		WORLD_BUFFER_FRAME,
		WORLD_BUFFER_VISIBILITY_CANDIDATES,
		WORLD_BUFFER_VISIBLE_ITEMS,
		WORLD_BUFFER_VISIBILITY_COUNTERS,
		WORLD_BUFFER_INDIRECT,
		WORLD_BUFFER_COUNT,
	} t3dWorldBufferId_t;

// From src/rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.h
typedef enum t3dVisibilityPipeline_e : uint32_t {
		PIPELINE_RESET = 0u,
		PIPELINE_CULL,
		PIPELINE_FINALIZE,
		PIPELINE_COUNT,
	} t3dVisibilityPipeline_t;

// From src/rendering/geometry/terrain_3d_clipmap_strip_contract.h
typedef enum t3dClipmapPartCode_e : uint16_t {
	T3D_CLIPMAP_PART_STATIC = 0u,
	T3D_CLIPMAP_PART_EDGE_X0 = 1u,
	T3D_CLIPMAP_PART_EDGE_X1 = 2u,
	T3D_CLIPMAP_PART_EDGE_Z0 = 3u,
	T3D_CLIPMAP_PART_EDGE_Z1 = 4u,
	T3D_CLIPMAP_PART_CODE_COUNT = 5u,
} t3dClipmapPartCode_t;

// From src/rendering/geometry/terrain_3d_clipmap_strip_contract.h
typedef enum t3dClipmapStripPartFlag_e : uint16_t {
	T3D_CLIPMAP_STRIP_FLAG_SERPENTINE_ROWS = 1u << 0u,
	T3D_CLIPMAP_STRIP_FLAG_LAYOUT_LOD0 = 1u << 1u,
	T3D_CLIPMAP_STRIP_FLAG_LAYOUT_OUTER_RING = 1u << 2u,
	T3D_CLIPMAP_STRIP_FLAG_ROLE_TILE = 1u << 3u,
	T3D_CLIPMAP_STRIP_FLAG_ROLE_TRIM_A = 1u << 4u,
	T3D_CLIPMAP_STRIP_FLAG_ROLE_TRIM_B = 1u << 5u,
	T3D_CLIPMAP_STRIP_FLAG_ROLE_FILL_A = 1u << 6u,
	T3D_CLIPMAP_STRIP_FLAG_ROLE_FILL_B = 1u << 7u,
	T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_X = 1u << 8u,
	T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_Z = 1u << 9u,
} t3dClipmapStripPartFlag_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dWorldBackground_e {
	NONE,
	FLAT,
	NOISE,
} t3dWorldBackground_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTextureFiltering_e {
	LINEAR_ANISOTROPIC,
	LINEAR,
	NEAREST_ANISOTROPIC,
	NEAREST,
} t3dTextureFiltering_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dUpdateFlags_e {
	UNIFORMS_ONLY = 0,
	TEXTURE_ARRAYS = 1 << 0,
	REGION_ARRAYS = 1 << 1,
	UPDATE_ARRAYS = TEXTURE_ARRAYS | REGION_ARRAYS,
	FULL_REBUILD = ( 1 << 2 ) | UPDATE_ARRAYS,
} t3dUpdateFlags_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dShaderPerformancePreset_e {
	SHADER_PRESET_CUSTOM,
	SHADER_PRESET_CINEMATIC,
	SHADER_PRESET_HIGH,
	SHADER_PRESET_RUNTIME_360,
	SHADER_PRESET_LOW,
	SHADER_PRESET_MINIMUM,
	// Appended so existing serialized preset values remain stable.
	SHADER_PRESET_XBOX_360_AUTHENTIC,
} t3dShaderPerformancePreset_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dShaderTier_e {
	SHADER_TIER_NEAR,
	SHADER_TIER_MIDDLE,
	SHADER_TIER_FAR,
	SHADER_TIER_COUNT,
} t3dShaderTier_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTerrainSamplingMode_e {
	TERRAIN_SAMPLING_ADAPTIVE,
	TERRAIN_SAMPLING_ALWAYS_SMOOTH,
	TERRAIN_SAMPLING_SINGLE_SAMPLE,
	// Appended so existing serialized values remain stable.
	TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED,
	TERRAIN_SAMPLING_DISTANCE_ADAPTIVE,
	TERRAIN_SAMPLING_ADAPTIVE_2_CORNER,
} t3dTerrainSamplingMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTerrainNormalMode_e {
	TERRAIN_NORMAL_ADAPTIVE,
	TERRAIN_NORMAL_HEIGHT_8_TAP,
	TERRAIN_NORMAL_GEOMETRY_DERIVATIVE,
	TERRAIN_NORMAL_DERIVED_TEXTURE,
	TERRAIN_NORMAL_HEIGHT_3_TAP,
	// Serialized compatibility aliases.
} t3dTerrainNormalMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dColorSamplingMode_e {
	COLOR_SAMPLING_AUTO,
	COLOR_SAMPLING_HARDWARE_FILTERED,
	COLOR_SAMPLING_EXACT_4_TAP,
} t3dColorSamplingMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dProjectionMode_e {
	PROJECTION_DISABLED,
	PROJECTION_SHARED_FRAGMENT,
	PROJECTION_EXACT_PER_CORNER,
} t3dProjectionMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTessellationMaterialMode_e {
	TESSELLATION_GEOMETRY_ONLY,
	TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT,
} t3dTessellationMaterialMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dSurfaceDataMode_e {
	SURFACE_DATA_AUTO,
	SURFACE_DATA_FULL,
	SURFACE_DATA_ALBEDO_ONLY,
} t3dSurfaceDataMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTextureSurfaceMode_e {
	TEXTURE_SURFACE_TEXTURED,
	TEXTURE_SURFACE_SILHOUETTE,
} t3dTextureSurfaceMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTerrainShadingMode_e {
	SHADING_BURLEY_GGX,
	SHADING_LAMBERT_GGX,
	SHADING_LAMBERT_DIFFUSE_ONLY,
	SHADING_UNSHADED,
	// Appended to preserve serialized values of the original four modes.
	SHADING_LAMBERT_VERTEX,
	// Non-PBR Lambert + Blinn-Phong response approximating common 2005-2013 terrain renderers.
	SHADING_XBOX_360_BLINN_PHONG,
} t3dTerrainShadingMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dMaterialSamplingQuality_e {
	MATERIAL_SAMPLING_FAST,
	MATERIAL_SAMPLING_FULL,
} t3dMaterialSamplingQuality_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dMiddleSurfaceSource_e {
	MIDDLE_SURFACE_BAKED_COMPOSITE,
	MIDDLE_SURFACE_RUNTIME_MATERIAL,
} t3dMiddleSurfaceSource_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTerrainNormalQuality_e {
	TERRAIN_NORMAL_QUALITY_GEOMETRY,
	TERRAIN_NORMAL_QUALITY_DETAILED,
} t3dTerrainNormalQuality_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTerrainColorQuality_e {
	TERRAIN_COLOR_QUALITY_FAST,
	TERRAIN_COLOR_QUALITY_EXACT,
} t3dTerrainColorQuality_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dSurfaceResponseQuality_e {
	SURFACE_RESPONSE_SIMPLE,
	SURFACE_RESPONSE_DETAILED,
} t3dSurfaceResponseQuality_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTierLightingModel_e {
	TIER_LIGHTING_UNSHADED,
	TIER_LIGHTING_LAMBERT,
	TIER_LIGHTING_BLINN_PHONG,
	TIER_LIGHTING_PBR,
} t3dTierLightingModel_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dTierDetilingQuality_e {
	TIER_DETILING_DISABLED,
	TIER_DETILING_ROTATION,
} t3dTierDetilingQuality_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dDetilingMode_e {
	DETILING_CONTINUOUS,
	DETILING_DISCRETE_8,
	DETILING_DISABLED,
	// Appended so existing serialized values remain stable.
	DETILING_TRANSLATION_ONLY,
	DETILING_DISCRETE_4,
	DETILING_DISCRETE_16,
} t3dDetilingMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dWorldNoiseShaderMode_e {
	WORLD_NOISE_DYNAMIC,
	WORLD_NOISE_CACHED_VERTEX_NORMALS,
	WORLD_NOISE_CACHED_FRAGMENT_NORMALS,
	WORLD_NOISE_PROCEDURAL_VERTEX_NORMALS,
	WORLD_NOISE_PROCEDURAL_FRAGMENT_NORMALS,
} t3dWorldNoiseShaderMode_t;

// From src/rendering/material/terrain_3d_material.h
typedef enum t3dPageTableBuildResult_e {
	PAGE_TABLE_READY,
	PAGE_TABLE_INVALID_TERRAIN_STATE,
	PAGE_TABLE_INVALID_DIMENSIONS,
	PAGE_TABLE_TEXTURE_ALLOCATION_FAILED,
} t3dPageTableBuildResult_t;

// From src/rendering/material/terrain_3d_material_schema.h
typedef enum t3dMaterialSchemaVisibility_e {
	T3D_MATERIAL_SCHEMA_ALWAYS = 0,
	T3D_MATERIAL_SCHEMA_WORLD_FLAT,
	T3D_MATERIAL_SCHEMA_AUTO_SHADER,
	T3D_MATERIAL_SCHEMA_DUAL_SCALING,
	T3D_MATERIAL_SCHEMA_MACRO_VARIATION,
	T3D_MATERIAL_SCHEMA_WORLD_NOISE,
	T3D_MATERIAL_SCHEMA_DEBUG_HEIGHTMAP,
	T3D_MATERIAL_SCHEMA_CONTOURS,
} t3dMaterialSchemaVisibility_t;

// From src/rendering/material/terrain_3d_material_schema.h
typedef enum t3dMaterialParameterId_e {
	T3D_MATERIAL_PARAM_FLAT_TERRAIN_NORMALS = 0,
	T3D_MATERIAL_PARAM_DISTANT_NORMAL_SCALE,
	T3D_MATERIAL_PARAM_BLEND_SHARPNESS,
	T3D_MATERIAL_PARAM_SILHOUETTE_COLOR,
	T3D_MATERIAL_PARAM_BIAS_DISTANCE,
	T3D_MATERIAL_PARAM_MIPMAP_BIAS,
	T3D_MATERIAL_PARAM_DEPTH_BLUR,
	T3D_MATERIAL_PARAM_GROUND_LEVEL,
	T3D_MATERIAL_PARAM_REGION_BLEND,
	T3D_MATERIAL_PARAM_AUTO_SLOPE,
	T3D_MATERIAL_PARAM_AUTO_HEIGHT_REDUCTION,
	T3D_MATERIAL_PARAM_AUTO_BASE_TEXTURE,
	T3D_MATERIAL_PARAM_AUTO_OVERLAY_TEXTURE,
	T3D_MATERIAL_PARAM_DUAL_SCALE_TEXTURE,
	T3D_MATERIAL_PARAM_DUAL_SCALE_REDUCTION,
	T3D_MATERIAL_PARAM_TRI_SCALE_REDUCTION,
	T3D_MATERIAL_PARAM_DUAL_SCALE_FAR,
	T3D_MATERIAL_PARAM_DUAL_SCALE_NEAR,
	T3D_MATERIAL_PARAM_MACRO_VARIATION1,
	T3D_MATERIAL_PARAM_MACRO_VARIATION2,
	T3D_MATERIAL_PARAM_MACRO_VARIATION_SLOPE,
	T3D_MATERIAL_PARAM_NOISE_TEXTURE,
	T3D_MATERIAL_PARAM_NOISE1_SCALE,
	T3D_MATERIAL_PARAM_NOISE1_ANGLE,
	T3D_MATERIAL_PARAM_NOISE1_OFFSET,
	T3D_MATERIAL_PARAM_NOISE2_SCALE,
	T3D_MATERIAL_PARAM_WORLD_NOISE_FRAGMENT_NORMALS,
	T3D_MATERIAL_PARAM_WORLD_NOISE_CACHE_ENABLED,
	T3D_MATERIAL_PARAM_WORLD_NOISE_CACHE_PERIOD,
	T3D_MATERIAL_PARAM_WORLD_NOISE_MAX_OCTAVES,
	T3D_MATERIAL_PARAM_WORLD_NOISE_MIN_OCTAVES,
	T3D_MATERIAL_PARAM_WORLD_NOISE_LOD_DISTANCE,
	T3D_MATERIAL_PARAM_WORLD_NOISE_SCALE,
	T3D_MATERIAL_PARAM_WORLD_NOISE_HEIGHT,
	T3D_MATERIAL_PARAM_WORLD_NOISE_OFFSET,
	T3D_MATERIAL_PARAM_HEIGHTMAP_BLACK_HEIGHT,
	T3D_MATERIAL_PARAM_HEIGHTMAP_WHITE_HEIGHT,
	T3D_MATERIAL_PARAM_CONTOUR_INTERVAL,
	T3D_MATERIAL_PARAM_CONTOUR_THICKNESS,
	T3D_MATERIAL_PARAM_CONTOUR_COLOR,
	T3D_MATERIAL_PARAM_COUNT,
	T3D_MATERIAL_PARAM_INVALID = -1,
} t3dMaterialParameterId_t;

// From src/rendering/geometry/terrain_3d_mesher.h
typedef enum t3dMeshType_e {
	LOD0_MESH,
	OUTER_RING_MESH,
	MESH_TYPE_COUNT,
} t3dMeshType_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainShaderPassClass_e {
	T3D_TERRAIN_SHADER_PASS_COLOR_GENERAL = 0u,
	T3D_TERRAIN_SHADER_PASS_COLOR_ONE_SUN = 1u,
	T3D_TERRAIN_SHADER_PASS_DEPTH_ONLY = 2u,
	T3D_TERRAIN_SHADER_PASS_DEPTH_NORMAL_ROUGHNESS = 3u,
	T3D_TERRAIN_SHADER_PASS_COUNT = 4u,
} t3dTerrainShaderPassClass_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainMaterialBackend_e {
	T3D_TERRAIN_MATERIAL_BACKEND_CANONICAL_VT_V3 = 0u,
	T3D_TERRAIN_MATERIAL_BACKEND_COMPATIBILITY_MIXED = 1u,
	T3D_TERRAIN_MATERIAL_BACKEND_COUNT = 2u,
} t3dTerrainMaterialBackend_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainShaderPath_e {
	// Normal runtime path. Legacy/editor material formats are not reachable.
	T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL = 0u,
	// Runtime/import fallback for mixed or uncooked material publication.
	T3D_TERRAIN_SHADER_PATH_COMPATIBILITY_MIXED = 1u,
	// Editor/debug path. Keeps visualization/decal behavior and mixed inputs.
	T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED = 2u,
	T3D_TERRAIN_SHADER_PATH_COUNT = 3u,
} t3dTerrainShaderPath_t;

// From src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.h
typedef enum t3dRdShaderId_e : uint32_t {
	T3D_RD_SHADER_TERRAIN_FORWARD_BASE = 0u,
	T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE = T3D_RD_SHADER_TERRAIN_FORWARD_BASE
	        + T3D_TERRAIN_SHADER_PATH_COUNT * 3u * T3D_RD_FORWARD_SHADER_VARIANT_MAX,
	T3D_RD_SHADER_NEAR_ALIAS = T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE
	        + T3D_TERRAIN_SHADER_PATH_COUNT * 3u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT,
	T3D_RD_SHADER_MIDDLE_ALIAS,
	T3D_RD_SHADER_FAR_ALIAS,
	T3D_RD_SHADER_TERRAIN_CULL,
	T3D_RD_SHADER_INSTANCE_FORWARD_BASE,
	T3D_RD_SHADER_INSTANCE_ALIAS = T3D_RD_SHADER_INSTANCE_FORWARD_BASE + T3D_RD_FORWARD_SHADER_VARIANT_MAX,
	T3D_RD_SHADER_INSTANCE_OCCLUDER_DEPTH,
	T3D_RD_SHADER_HLOD_CULL,
	T3D_RD_SHADER_INSTANCE_CULL,
	T3D_RD_SHADER_WORLD_HIZ_REDUCE,
	T3D_RD_SHADER_WORLD_VISIBILITY_RESET,
	T3D_RD_SHADER_WORLD_VISIBILITY_CULL,
	T3D_RD_SHADER_WORLD_VISIBILITY_FINALIZE,
	T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE,
	T3D_RD_SHADER_WORLD_RASTER_MESH_BASE = T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE + T3D_RD_FORWARD_SHADER_VARIANT_MAX,
	T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW = T3D_RD_SHADER_WORLD_RASTER_MESH_BASE + T3D_RD_FORWARD_SHADER_VARIANT_MAX,
	T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW,
	T3D_RD_SHADER_OCEAN_COLOR,
	T3D_RD_SHADER_OCEAN_SHADOW,
	T3D_RD_SHADER_BRUSH_COMPUTE,
	T3D_RD_SHADER_DERIVED_COMPUTE,
	T3D_RD_SHADER_DISPLACEMENT_COMPUTE,
	T3D_RD_SHADER_INSTANCE_FEEDBACK_BASE,
	T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE = T3D_RD_SHADER_INSTANCE_FEEDBACK_BASE + T3D_RD_FORWARD_COLOR_VARIANT_COUNT,
	T3D_RD_SHADER_COUNT = T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE + T3D_RD_FORWARD_COLOR_VARIANT_COUNT,
} t3dRdShaderId_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainShippingFeatureClass_e {
	T3D_TERRAIN_SHIPPING_FEATURE_AUTO_SHADER = 1u << 0u,
	T3D_TERRAIN_SHIPPING_FEATURE_DUAL_SCALE = 1u << 1u,
	T3D_TERRAIN_SHIPPING_FEATURE_MEGAVT = 1u << 2u,
	T3D_TERRAIN_SHIPPING_FEATURE_MASK = T3D_TERRAIN_SHIPPING_FEATURE_AUTO_SHADER | T3D_TERRAIN_SHIPPING_FEATURE_DUAL_SCALE
	        | T3D_TERRAIN_SHIPPING_FEATURE_MEGAVT,
	// Compatibility/editor paths deliberately preserve runtime feature tests.
	T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME = 0xffu,
} t3dTerrainShippingFeatureClass_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainLightingModelClass_e {
	T3D_TERRAIN_LIGHTING_MODEL_UNSHADED = 0u,
	T3D_TERRAIN_LIGHTING_MODEL_LAMBERT = 1u,
	T3D_TERRAIN_LIGHTING_MODEL_BLINN_PHONG = 2u,
	T3D_TERRAIN_LIGHTING_MODEL_PBR = 3u,
	T3D_TERRAIN_LIGHTING_MODEL_RUNTIME = 0xffu,
} t3dTerrainLightingModelClass_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainSceneIntegrationClass_e {
	T3D_TERRAIN_SCENE_INTEGRATION_DYNAMIC = 0u,
	T3D_TERRAIN_SCENE_INTEGRATION_NONE = 1u,
	T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME = 0xffu,
} t3dTerrainSceneIntegrationClass_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainDnrOutputClass_e {
	T3D_TERRAIN_DNR_OUTPUT_NORMAL = 1u << 0u,
	T3D_TERRAIN_DNR_OUTPUT_ROUGHNESS = 1u << 1u,
	T3D_TERRAIN_DNR_OUTPUT_MASK = T3D_TERRAIN_DNR_OUTPUT_NORMAL | T3D_TERRAIN_DNR_OUTPUT_ROUGHNESS,
	T3D_TERRAIN_DNR_OUTPUT_RUNTIME = 0xffu,
} t3dTerrainDnrOutputClass_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
typedef enum t3dTerrainMiddleSurfaceClass_e {
	T3D_TERRAIN_MIDDLE_SURFACE_DETAILED = 0u,
	T3D_TERRAIN_MIDDLE_SURFACE_COMPOSITE = 1u,
	T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME = 0xffu,
} t3dTerrainMiddleSurfaceClass_t;

// From src/simd/core/terrain_3d_simd.h
typedef enum t3dSimdBackend_e : uint8_t {
	T3D_SIMD_SCALAR = 0,
	T3D_SIMD_SSE2,
	T3D_SIMD_AVX_F16C,
	T3D_SIMD_AVX2_F16C,
	T3D_SIMD_NEON,
} t3dSimdBackend_t;

// From src/streaming/policy/terrain_3d_adaptive_budget_policy.h
typedef enum t3dAdaptiveBudgetReason_e {
	TERRAIN3D_ADAPTIVE_REASON_DISABLED = 0,
	TERRAIN3D_ADAPTIVE_REASON_INITIALIZED,
	TERRAIN3D_ADAPTIVE_REASON_HARD_LIMIT,
	TERRAIN3D_ADAPTIVE_REASON_BACKLOG_RECOVERY,
	TERRAIN3D_ADAPTIVE_REASON_UNDER_TARGET_NO_BACKLOG,
	TERRAIN3D_ADAPTIVE_REASON_STABLE,
} t3dAdaptiveBudgetReason_t;

// From src/Terrain3DConstants.h
typedef enum t3dGpuRegionSlotState_e : uint8_t {
	TERRAIN3D_GPU_SLOT_FREE = 0u,
	TERRAIN3D_GPU_SLOT_STAGING = 1u,
	TERRAIN3D_GPU_SLOT_READY = 2u,
	TERRAIN3D_GPU_SLOT_MAPPED = 3u,
	TERRAIN3D_GPU_SLOT_RETIRED = 4u,
} t3dGpuRegionSlotState_t;

// From src/streaming/cache/terrain_3d_gpu_region_cache.h
typedef enum t3dGpuRegionPendingPublication_e : uint8_t {
	TERRAIN3D_GPU_PENDING_PUBLICATION_NONE = 0u,
	TERRAIN3D_GPU_PENDING_PUBLICATION_NEW = 1u,
	TERRAIN3D_GPU_PENDING_PUBLICATION_REPUBLISH = 2u,
} t3dGpuRegionPendingPublication_t;

// From src/streaming/cache/terrain_3d_instance_region_cache.h
typedef enum t3dInstanceRegionCacheEvictionReason_e : uint8_t {
	TERRAIN3D_INSTANCE_REGION_EVICTION_NONE = 0u,
	TERRAIN3D_INSTANCE_REGION_EVICTION_EXPLICIT = 1u,
	TERRAIN3D_INSTANCE_REGION_EVICTION_INVALIDATE = 2u,
	TERRAIN3D_INSTANCE_REGION_EVICTION_RESET = 3u,
	TERRAIN3D_INSTANCE_REGION_EVICTION_SLOT_PRESSURE = 4u,
	TERRAIN3D_INSTANCE_REGION_EVICTION_DEMAND_BUDGET = 5u,
	TERRAIN3D_INSTANCE_REGION_EVICTION_BUDGET_RECOVERY = 6u,
	TERRAIN3D_INSTANCE_REGION_EVICTION_SELECTION_CHANGE = 7u,
} t3dInstanceRegionCacheEvictionReason_t;

// From src/streaming/runtime/terrain_3d_node_streamer.h
typedef enum t3dStreamedNodeFlags_e {
	NODE_FLAG_NONE = 0,
	NODE_FLAG_DISABLED = 1 << 0,
	// Static streamed nodes never participate in transform-based ownership migration.
	// The default remains movable to preserve serialized behavior from older scenes.
	NODE_FLAG_STATIC = 1 << 1,
	// Static/render-only scene that may be replaced by an HLOD proxy.
	NODE_FLAG_HLOD_VISUAL = 1 << 2,
} t3dStreamedNodeFlags_t;

// From src/streaming/runtime/terrain_3d_node_streamer.h
typedef enum t3dSpawnActivationResult_e {
	SPAWN_ACTIVATION_PENDING,
	SPAWN_ACTIVATION_CONSUMED,
	SPAWN_ACTIVATION_FAILED,
} t3dSpawnActivationResult_t;

// From src/streaming/runtime/terrain_3d_runtime_page_store.h
typedef enum t3dRuntimePageConsumer_e : uint16_t {
	RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT = 1u << 0,
	RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT = 1u << 1,
	RUNTIME_PAGE_CONSUMER_GPU_DETAIL = 1u << 2,
	RUNTIME_PAGE_CONSUMER_INSTANCE_STORE = 1u << 3,
	RUNTIME_PAGE_CONSUMER_CPU_MAPS = 1u << 4,
	RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS = 1u << 5,
	RUNTIME_PAGE_CONSUMER_COLLISION = 1u << 6,
	RUNTIME_PAGE_CONSUMER_NAVIGATION = 1u << 7,
	RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW = 1u << 8,
} t3dRuntimePageConsumer_t;

// From src/streaming/runtime/terrain_3d_runtime_page_store.h
typedef enum t3dRuntimePageProbeState_e : uint8_t {
	RUNTIME_PAGE_PROBE_MISSING = 0u,
	RUNTIME_PAGE_PROBE_CURRENT = 1u,
	RUNTIME_PAGE_PROBE_LEGACY = 2u,
	RUNTIME_PAGE_PROBE_FUTURE = 3u,
	RUNTIME_PAGE_PROBE_INVALID = 4u,
} t3dRuntimePageProbeState_t;

// From src/streaming/policy/terrain_3d_runtime_profile.h
typedef enum t3dRuntimeProfileEvent_e : uint32_t {
	TERRAIN3D_PROFILE_EVENT_NONE = 0u,
	TERRAIN3D_PROFILE_EVENT_CAMERA_MOVED = 1u << 0u,
	TERRAIN3D_PROFILE_EVENT_GPU_UPLOAD = 1u << 1u,
	TERRAIN3D_PROFILE_EVENT_GPU_EVICTION = 1u << 2u,
	TERRAIN3D_PROFILE_EVENT_GPU_MISSING_REQUIRED = 1u << 3u,
	TERRAIN3D_PROFILE_EVENT_COOKED_PAGE_MISS = 1u << 4u,
	TERRAIN3D_PROFILE_EVENT_RUNTIME_PAGE_PREPARE = 1u << 5u,
	TERRAIN3D_PROFILE_EVENT_CLIPMAP_TELEPORT = 1u << 6u,
	TERRAIN3D_PROFILE_EVENT_NODE_LOAD_FAILURE = 1u << 7u,
	TERRAIN3D_PROFILE_EVENT_FAR_ATLAS_WORK = 1u << 8u,
	TERRAIN3D_PROFILE_EVENT_CPU_DEMAND_LOAD = 1u << 9u,
	TERRAIN3D_PROFILE_EVENT_INSTANCE_BACKLOG = 1u << 10u,
} t3dRuntimeProfileEvent_t;

// From src/streaming/policy/terrain_3d_runtime_profile.h
typedef enum t3dRuntimeProfileStall_e : uint32_t {
	TERRAIN3D_PROFILE_STALL_NONE = 0u,
	TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET = 1u << 0u,
	TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING = 1u << 1u,
	TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE = 1u << 2u,
	TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY = 1u << 3u,
	TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET = 1u << 4u,
	TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET = 1u << 5u,
	TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET = 1u << 6u,
	TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET = 1u << 7u,
	TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET = 1u << 8u,
	TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET = 1u << 9u,
	TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET = 1u << 10u,
	TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY = 1u << 11u,
	TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET = 1u << 12u,
	TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY = 1u << 13u,
	TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET = 1u << 14u,
} t3dRuntimeProfileStall_t;

// From src/streaming/policy/terrain_3d_streaming_policy.h
typedef enum t3dEditorExecutionMode_e
{
	EDITOR_EXECUTION_AUTHORING = 0,
	EDITOR_EXECUTION_RUNTIME_PROFILE = 1,
} t3dEditorExecutionMode_t;

// From src/streaming/policy/terrain_3d_streaming_policy.h
typedef enum t3dRegionStreamingState_e : uint8_t
{
	TERRAIN3D_REGION_STREAMING_UNLOADED = 0,
	TERRAIN3D_REGION_STREAMING_COLD = 1,
	TERRAIN3D_REGION_STREAMING_HOT = 2,
	TERRAIN3D_REGION_STREAMING_ACTIVE = 3,
} t3dRegionStreamingState_t;

// From src/terrain_3d.h
typedef enum t3dDebugLevel_e
{
	MESG = -2,   // Always print except in release builds
	WARN = -1,   // Always print except in release builds
	ERROR = 0,   // Always print except in release builds
	INFO = 1,    // Print every function call and important entries
	DEBUG = 2,   // Print details within functions
	EXTREME = 3, // Continuous operations like snapping
} t3dDebugLevel_t;

// From src/terrain_3d.h
typedef enum t3dRuntimeLifecycleState_e : uint8_t
{
	TERRAIN3D_RUNTIME_COLD = 0u,
	TERRAIN3D_RUNTIME_INITIALIZING,
	TERRAIN3D_RUNTIME_ACTIVE,
	TERRAIN3D_RUNTIME_DEACTIVATING,
	TERRAIN3D_RUNTIME_DETACHED,
	TERRAIN3D_RUNTIME_SHUTTING_DOWN,
	TERRAIN3D_RUNTIME_DRAIN_FAILED,
} t3dRuntimeLifecycleState_t;

// From src/terrain_3d.h
typedef enum t3dPerformanceTarget_e
{
	PERFORMANCE_TARGET_CUSTOM,
	PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL,
} t3dPerformanceTarget_t;

// From src/terrain_3d.h
typedef enum t3dPerformanceProfileStatus_e
{
	PERFORMANCE_PROFILE_CUSTOM,
	PERFORMANCE_PROFILE_EXACT,
	PERFORMANCE_PROFILE_MODIFIED,
} t3dPerformanceProfileStatus_t;

// From src/terrain_3d.h
typedef enum t3dEnvironmentShadowQuality_e
{
	ENVIRONMENT_SHADOW_LOW,
	ENVIRONMENT_SHADOW_NORMAL,
	ENVIRONMENT_SHADOW_HIGH,
	ENVIRONMENT_SHADOW_ULTRA,
} t3dEnvironmentShadowQuality_t;

// From src/terrain_3d.h
typedef enum t3dRegionSize_e
{
	SIZE_128 = 128,
} t3dRegionSize_t;

// From src/terrain_3d.h
typedef enum t3dOceanPerformancePreset_e
{
	OCEAN_PRESET_CUSTOM,
	OCEAN_PRESET_BORDER_MINIMUM,
	OCEAN_PRESET_BORDER_BALANCED,
	OCEAN_PRESET_HIGH,
	OCEAN_PRESET_CINEMATIC,
} t3dOceanPerformancePreset_t;

// From src/terrain_3d.h
typedef enum t3dOceanRefractionMode_e
{
	OCEAN_REFRACTION_OFF,
	OCEAN_REFRACTION_DEPTH_TINT,
	OCEAN_REFRACTION_FULL
} t3dOceanRefractionMode_t;

// From src/terrain_3d.h
typedef enum t3dOceanFoamMode_e
{
	OCEAN_FOAM_OFF,
	OCEAN_FOAM_SHORE,
	OCEAN_FOAM_FULL
} t3dOceanFoamMode_t;

// From src/terrain_3d.h
typedef enum t3dOceanScatteringMode_e
{
	OCEAN_SCATTERING_OFF,
	OCEAN_SCATTERING_APPROXIMATE,
	OCEAN_SCATTERING_FULL
} t3dOceanScatteringMode_t;

// From src/terrain_3d.h
typedef enum t3dOceanShadingMode_e
{
	OCEAN_SHADING_LAMBERT_DIFFUSE,
	OCEAN_SHADING_LAMBERT_GGX,
	OCEAN_SHADING_BURLEY_GGX,
	OCEAN_SHADING_UNSHADED,
	OCEAN_SHADING_LAMBERT_VERTEX
} t3dOceanShadingMode_t;

// POD/data records.
// From src/Terrain3DConstants.h
typedef struct t3dPerformanceProfileSetting_s {
	t3dPerformanceProfileOwner_t m_owner;
	t3dPerformanceProfileValueKind_t m_kind;
	const char *m_pszProperty;
	int64_t m_nIntValue;
	double m_fFloatValue;
	bool m_bBoolValue;
} t3dPerformanceProfileSetting_t;

// From src/Terrain3DConstants.h
typedef struct t3dRegionCoord_s {
	int8_t m_x;
	int8_t m_y;
} t3dRegionCoord_t;

// From src/assets/catalog/terrain_3d_assets.h
typedef struct t3dTextureSettingsBlock_s {
	Color m_szColors[T3D_MAX_TEXTURE_ASSETS]{};
	float m_szNormalDepths[T3D_MAX_TEXTURE_ASSETS]{};
	float m_szAoStrengths[T3D_MAX_TEXTURE_ASSETS]{};
	float m_szAoLightAffects[T3D_MAX_TEXTURE_ASSETS]{};
	float m_szRoughnessMods[T3D_MAX_TEXTURE_ASSETS]{};
	float m_szMetallics[T3D_MAX_TEXTURE_ASSETS]{};
	float m_szUvScales[T3D_MAX_TEXTURE_ASSETS]{};
	Vector2 m_szDetiles[T3D_MAX_TEXTURE_ASSETS]{};
	Vector2 m_szDisplacements[T3D_MAX_TEXTURE_ASSETS]{};
	int m_nCount = 0;
} t3dTextureSettingsBlock_t;

// From src/assets/catalog/terrain_3d_assets.h
typedef struct t3dTextureArrayLayout_s {
	Vector2i m_vSize = Vector2i( 0, 0 );
	Image::Format m_eFormat = Image::FORMAT_MAX;
	bool m_bMipmaps = true;
	bool m_bNeedsNormalization = false;
} t3dTextureArrayLayout_t;

// From src/authoring/baking/terrain_3d_autoshader_baker.cpp
typedef struct t3dMetricNeeds_s {
	bool m_bSlope = false;
	bool m_bCurvature = false;
	bool m_bConcavity = false;
	bool m_bAspect = false;
	bool m_bFlow = false;
	bool m_bDeposition = false;
	bool m_bWaterDistance = false;
	bool m_bNoise = false;
	bool m_bBiome = false;
} t3dMetricNeeds_t;

// From src/authoring/baking/terrain_3d_autoshader_baker.cpp
typedef struct t3dMetricBuildConfig_s {
	float m_fCurvatureScale = 1.0f;
	float m_fWaterLevel = 0.0f;
	float m_fNoiseScale = 0.01f;
	int m_nNoiseOctaves = 4;
	float m_fNoiseLacunarity = 2.0f;
	float m_fNoiseGain = 0.5f;
	uint32_t m_nNoiseSeed = 1337u;
} t3dMetricBuildConfig_t;

// From src/authoring/editor/terrain_3d_editor.h
typedef struct t3dAxisSample_s
{
	real_t m_fOffset = 0.f;
	real_t m_fGlobalPosition = 0.f;
	int32_t m_nRegionLocation = 0;
	int32_t m_nMapPixel = 0;
	real_t m_fCenteredUv = 0.f;
} t3dAxisSample_t;

// From src/authoring/editor/terrain_3d_editor.h
typedef struct t3dComputeWrite_s
{
	size_t m_iRegionIndex = 0u;
	Vector2i m_mapPixel = Vector2i( 0, 0 );
	uint32_t m_nSourceValue = 0u;
	Vector3 m_worldPosition = Vector3( 0.f, 0.f, 0.f );
} t3dComputeWrite_t;

// From src/authoring/erosion/terrain_3d_erosion_job.cpp
typedef struct t3dErosionFinalStatistics_s {
	double m_fEroded = 0.0;
	double m_fDeposited = 0.0;
	float m_fMaximumLowering = 0.0f;
	float m_fMaximumRaising = 0.0f;
} t3dErosionFinalStatistics_t;

// From src/authoring/erosion/terrain_3d_erosion_job.cpp
typedef struct t3dTransitionScore_s {
				t3dMaterialTransitionKind_t m_kind;
				float m_fScore;
			} t3dTransitionScore_t;

// From src/authoring/erosion/terrain_3d_erosion_job.h
typedef struct t3dTextureErosionProfile_s {
	bool m_bValid = false;
	bool m_bEnabled = true;
	float m_fHardness = -1.0f;
	float m_fHydraulicErodibility = 1.0f;
	float m_fWindErodibility = 1.0f;
	float m_fWaveErodibility = 1.0f;
	float m_fWeatheringSusceptibility = 1.0f;
	float m_fOxidationSusceptibility = 1.0f;
	float m_fSaltSusceptibility = 1.0f;
	float m_fWetDrySusceptibility = 1.0f;
	float m_fInfiltrationMultiplier = 1.0f;
	float m_fTrafficErodibility = 1.0f;
	float m_fCohesion = 0.0f;
	float m_fGrainSize = 0.5f;
	int32_t m_nExposedTextureId = -1;
	int32_t m_nWeatheredTextureId = -1;
	int32_t m_nOxidizedTextureId = -1;
	int32_t m_nCoarseDepositTextureId = -1;
	int32_t m_nFineDepositTextureId = -1;
	int32_t m_nSaltWeatheredTextureId = -1;
	int32_t m_nPavementTextureId = -1;
	int32_t m_nVarnishedTextureId = -1;
} t3dTextureErosionProfile_t;

// From src/authoring/erosion/terrain_3d_erosion_job.h
struct RegionNeighbors {
		int16_t m_ids[T3D_EROSION_DIRECTION_COUNT]{};
	};

// From src/authoring/baking/terrain_3d_landform_baker.cpp
typedef struct t3dBasinProfile_s {
	float m_fPathLength = 0.0f;
	float m_fHeightSlope = 0.0f;
	float m_fHeightIntercept = 0.0f;
	bool m_bValid = false;
} t3dBasinProfile_t;

// From src/authoring/baking/terrain_3d_landform_baker.cpp
typedef struct t3dCollisionSection_s {
		Vector3 m_top;
		Vector3 m_bottom;
	} t3dCollisionSection_t;

// From src/authoring/baking/terrain_3d_landform_baker.cpp
typedef struct t3dCrossSection_s {
		Vector3 m_highGround;
		Vector3 m_frontTop;
		Vector3 m_frontUpper;
		Vector3 m_frontLower;
		Vector3 m_frontToe;
		Vector3 m_lowGround;
		float m_fU = 0.0f;
		float m_fFaceHeight = 1.0f;
	} t3dCrossSection_t;

// From src/authoring/vt/build/terrain_3d_megatexture_cooker.h
typedef struct t3dMegatexturePageAddress_s {
	uint32_t m_nMip;
	uint32_t m_nPageX;
	uint32_t m_nPageY;
} t3dMegatexturePageAddress_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.cpp
typedef struct t3dVTCookStagedPayload_s {
	uint64_t m_nContentSignature;
	uint8_t *m_pBytes;
	uint32_t m_nBytes;
	uint32_t m_nFormat;
	uint32_t m_nFlags;
	uint32_t m_nChecksum;
} t3dVTCookStagedPayload_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.cpp
typedef struct t3dVTCookRawTileCacheEntryV3_s {
	uint64_t m_nHashA;
	uint64_t m_nHashB;
	uint32_t m_nRawBytes;
	uint32_t m_nFamily;
	uint32_t m_nChannel;
	uint32_t m_nPayloadId;
	uint32_t m_nFlags;
} t3dVTCookRawTileCacheEntryV3_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTCookerSettings_s {
	uint32_t m_nPageInterior;
	uint32_t m_nPageGutter;
	uint32_t m_nClusterTargetPages;
	uint32_t m_nClusterTargetBytes;
	uint32_t m_nOuterCompression;
	uint32_t m_nMipQuality;
	uint32_t m_nCompressionQuality;
} t3dVTCookerSettings_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTEncodeTileRequest_s {
	const uint8_t *m_pSourcePixels;
	uint32_t m_nSourceBytes;
	uint32_t m_nChannel;
	uint32_t m_nFormat;
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint32_t m_nComponents;
	uint32_t m_nQuality;
	uint32_t m_nContractVersion;
	uint8_t m_nAlphaThreshold = 0u; // Zero disables cutoff-constrained encoding.
	uint8_t *m_pDestination;
	uint32_t m_nDestinationCapacity;
} t3dVTEncodeTileRequest_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef bool ( *t3dVTEncodeTileFn )( void *pUser, const t3dVTEncodeTileRequest_s &rRequest, uint32_t &rWrittenBytes );

typedef struct t3dVTEncoderInterface_s {
	void *m_pUser;
	t3dVTEncodeTileFn m_pEncodeTile;
} t3dVTEncoderInterface_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTCookAllocator_s {
	void *m_pUser;
	t3dVTCookAllocateFn m_pAllocate;
	t3dVTCookFreeFn m_pFree;
} t3dVTCookAllocator_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTCookClusterCodec_s {
	void *m_pUser;
	t3dVTCookClusterBoundFn m_pBound;
	t3dVTCookClusterEncodeFn m_pEncode;
} t3dVTCookClusterCodec_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTCookSparseStatsV3_s {
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nTailCount;
	uint32_t m_nUniquePayloadCount;
	uint32_t m_nRawDedupHits;
	uint32_t m_nEncodeCalls;
	uint32_t m_nChannelPageCount[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nPhysicalFamilyPayloadCount[T3D_VT_PHYSICAL_FORMAT_COUNT];
	uint64_t m_nContentSignature;
} t3dVTCookSparseStatsV3_t;

// From src/authoring/vt/io/terrain_3d_vt_godot_cooker.h
typedef struct t3dVTGodotSourceChannelV3_s {
	uint32_t m_nSourceWidth;
	uint32_t m_nSourceHeight;
	uint32_t m_nSourceExtent;
	uint32_t m_nReserved0;
	uint64_t m_nSourceFingerprint;
	uint8_t m_bAuthored;
	uint8_t m_nLogicalChannel;
	uint8_t m_nConsumer;
	uint8_t m_nWrapMode;
	uint8_t m_bAlphaScissor;
	uint8_t m_nAlphaThreshold;
	uint8_t m_nReserved1[2];
} t3dVTGodotSourceChannelV3_t;

// From src/authoring/vt/io/terrain_3d_vt_godot_cooker.h
typedef struct t3dVTEditorCookTelemetryV3_s {
	uint32_t m_nStage;
	uint32_t m_nDirtyAllocations;
	uint32_t m_nAllocationCacheHits;
	uint32_t m_nAllocationCacheMisses;
	uint32_t m_nChannelCacheHits;
	uint32_t m_nChannelCacheMisses;
	uint64_t m_nExtractUsec;
	uint64_t m_nFilterUsec;
	uint64_t m_nPageUsec;
	uint64_t m_nEncodeUsec;
	uint64_t m_nPackUsec;
} t3dVTEditorCookTelemetryV3_t;

// From src/authoring/vt/io/terrain_3d_vt_pack_writer.h
typedef struct t3dVTPackAllocationKeyRecordV3_s {
	uint64_t m_nStableKey;
	uint32_t m_nAllocationId;
	uint32_t m_nContentId;
} t3dVTPackAllocationKeyRecordV3_t;

// From src/authoring/vt/io/terrain_3d_vt_pack_writer.h
typedef struct t3dVTPackPayloadRecordV3_s {
	uint64_t m_nContentSignature;
	uint32_t m_nFormat;
	uint32_t m_nClusterId;
	uint32_t m_nClusterOffset;
	uint32_t m_nStoredBytes;
	uint32_t m_nDecodedBytes;
	uint32_t m_nCodec;
	uint32_t m_nChecksum;
	uint32_t m_nFlags;
	uint32_t m_nReserved;
} t3dVTPackPayloadRecordV3_t;

// From src/authoring/vt/io/terrain_3d_vt_pack_writer.h
typedef struct t3dVTPackClusterRecordV3_s {
	uint64_t m_nPayloadDataOffset;
	uint64_t m_nContentSignature;
	uint32_t m_nStoredBytes;
	uint32_t m_nDecodedBytes;
	uint32_t m_nFirstPayload;
	uint32_t m_nPayloadCount;
	uint32_t m_nCodec;
	uint32_t m_nChecksum;
} t3dVTPackClusterRecordV3_t;

// From src/authoring/vt/io/terrain_3d_vt_pack_writer.h
typedef struct t3dVTPackHeaderV3_s {
	uint32_t m_nMagic;
	uint32_t m_nVersion;
	uint32_t m_nHeaderBytes;
	uint32_t m_nFlags;
	uint32_t m_nAllocationCount;
	uint32_t m_nChannelAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nPayloadCount;
	uint32_t m_nClusterCount;
	uint32_t m_nTailCount;
	uint32_t m_nChannelCount;
	uint32_t m_nInteriorTexels;
	uint32_t m_nGutterTexels;
	uint32_t m_nPhysicalTexels;
	uint32_t m_nPackageGeneration;
	uint32_t m_nHeaderChecksum;
	uint32_t m_nQualityViewCount;
	uint32_t m_nChannelPageCount[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nChannelTailCount[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nFormatFamilyCount;
	uint32_t m_nFullySupportedProfileMask;
	uint64_t m_nAllocationOffset;
	uint64_t m_nAllocationKeyOffset;
	uint64_t m_nChannelAllocationOffset;
	uint64_t m_nPageOffset;
	uint64_t m_nPayloadDirectoryOffset;
	uint64_t m_nClusterDirectoryOffset;
	uint64_t m_nTailMetadataOffset;
	uint64_t m_nQualityViewOffset;
	uint64_t m_nFormatFamilyOffset;
	uint64_t m_nPayloadDataOffset;
	uint64_t m_nPayloadDataBytes;
	uint64_t m_nContentSignature;
	uint64_t m_nPackageBytes;
	uint64_t m_nReserved[4];
} t3dVTPackHeaderV3_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTChannelAllocation_s {
	uint32_t m_nBaseVirtualPage;
	uint32_t m_nPageCount;
	uint32_t m_nTailIndex;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nFlags;
} t3dVTChannelAllocation_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPackAllocationRecordV3_s {
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint16_t m_nFlags;
	uint16_t m_nChannelMask;
	uint8_t m_nHighestAvailableProfile;
	uint8_t m_nAvailableProfileMask;
	uint8_t m_nConsumer;
	uint8_t m_nReserved0;
} t3dVTPackAllocationRecordV3_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPackPageRecordV3_s {
	uint32_t m_nVirtualPage;
	uint32_t m_nParentPage;
	uint32_t m_nAllocationId;
	uint32_t m_nPayloadId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint16_t m_nMip;
	uint16_t m_nFlags;
	uint16_t m_nPageX;
	uint16_t m_nPageY;
	uint16_t m_nChannel;
	uint16_t m_nReserved;
} t3dVTPackPageRecordV3_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPackTailRecordV3_s {
	uint64_t m_nContentSignature;
	uint32_t m_nCanonicalAllocationId;
	uint32_t m_nPayloadId;
	uint32_t m_nContentId;
	uint16_t m_nFirstMip;
	uint16_t m_nMipCount;
	uint16_t m_nFirstMipWidth;
	uint16_t m_nFirstMipHeight;
	uint16_t m_nAtlasInteriorWidth;
	uint16_t m_nAtlasInteriorHeight;
	uint16_t m_nChannel;
	uint16_t m_nFlags;
	uint32_t m_nLayout;
} t3dVTPackTailRecordV3_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPackQualityViewV3_s {
	uint32_t m_nAllocationId;
	uint8_t m_nProfile;
	uint8_t m_nChannelMask;
	uint8_t m_nBaseMip[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint8_t m_nMaxMip[T3D_VT_LOGICAL_CHANNEL_COUNT];
} t3dVTPackQualityViewV3_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPackFormatFamilyRecordV3_s {
	uint8_t m_nLogicalChannel;
	uint8_t m_nPhysicalFamily;
	uint8_t m_nSemantic;
	uint8_t m_nFlags;
	uint32_t m_nReserved;
} t3dVTPackFormatFamilyRecordV3_t;

// From src/authoring/vt/io/terrain_3d_vt_pack_writer.h
typedef struct t3dVTPackSourceV3_s {
	const t3dVTPackAllocationRecordV3_s *m_pAllocations;
	uint32_t m_nAllocationCount;
	const t3dVTPackAllocationKeyRecordV3_t *m_pAllocationKeys;
	const t3dVTChannelAllocation_s *m_pChannelAllocations;
	uint32_t m_nChannelAllocationCount;
	const t3dVTPackPageRecordV3_s *m_pPages;
	uint32_t m_nPageCount;
	const t3dVTPackPayloadRecordV3_t *m_pPayloads;
	uint32_t m_nPayloadCount;
	const t3dVTPackClusterRecordV3_t *m_pClusters;
	uint32_t m_nClusterCount;
	const t3dVTPackTailRecordV3_s *m_pTails;
	uint32_t m_nTailCount;
	const t3dVTPackQualityViewV3_s *m_pQualityViews;
	uint32_t m_nQualityViewCount;
	const t3dVTPackFormatFamilyRecordV3_s *m_pFormatFamilies;
	uint32_t m_nFormatFamilyCount;
	const uint8_t *m_pPayloadData;
	uint64_t m_nPayloadDataBytes;
	uint32_t m_nPackageGeneration;
} t3dVTPackSourceV3_t;

// From src/authoring/vt/io/terrain_3d_vt_pack_writer.h
typedef struct t3dVTPackViewV3_s {
	const t3dVTPackHeaderV3_t *m_pHeader;
	const t3dVTPackAllocationRecordV3_s *m_pAllocations;
	const t3dVTPackAllocationKeyRecordV3_t *m_pAllocationKeys;
	const t3dVTChannelAllocation_s *m_pChannelAllocations;
	const t3dVTPackPageRecordV3_s *m_pPages;
	const t3dVTPackPayloadRecordV3_t *m_pPayloads;
	const t3dVTPackClusterRecordV3_t *m_pClusters;
	const t3dVTPackTailRecordV3_s *m_pTails;
	const t3dVTPackQualityViewV3_s *m_pQualityViews;
	const t3dVTPackFormatFamilyRecordV3_s *m_pFormatFamilies;
	const uint8_t *m_pPayloadData;
} t3dVTPackViewV3_t;

// From src/authoring/vt/build/terrain_3d_vt_page_builder.h
typedef struct t3dVTCookImageView_s {
	const uint8_t *m_pPixels;
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint32_t m_nComponents;
	uint32_t m_nRowBytes;
} t3dVTCookImageView_t;

// From src/authoring/vt/build/terrain_3d_vt_page_builder.h
typedef struct t3dVTMipLevelLayout_s {
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint64_t m_nOffset;
	uint64_t m_nBytes;
} t3dVTMipLevelLayout_t;

// From src/authoring/vt/build/terrain_3d_vt_reference_encoder.cpp
typedef struct t3dVTReferenceBitWriter_s {
	uint8_t *m_pBytes;
	uint32_t m_nBit;
} t3dVTReferenceBitWriter_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef struct t3dWorldConversionPolicy_s {
	t3dWorldMode_t eMode;
	uint32_t nDefaultTier;
	t3dWorldMaterialPolicy_t eMaterialPolicy;
	t3dWorldTransparencyPolicy_t eTransparencyPolicy;
	bool bConvertMeshInstances;
	bool bConvertMultiMeshInstances;
	bool bConvertDynamicObjects;
	bool bConvertHiddenObjects;
	float fCellSize;
} t3dWorldConversionPolicy_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef struct t3dWorldCandidateDesc_s {
	uint64_t nSourceId;
	t3dWorldCandidateKind_t eKind;
	uint32_t nTier;
	bool bExplicitInclude;
	bool bExplicitExclude;
	bool bVisible;
	bool bStatic;
	bool bHasMesh;
	bool bMaterialSupported;
	bool bSortedTransparency;
	bool bTwoSided;
} t3dWorldCandidateDesc_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef struct t3dWorldClassification_s {
	t3dWorldOwnership_t eOwnership;
	t3dWorldConversionReason_t eReason;
	uint32_t nTier;
} t3dWorldClassification_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef struct t3dWorldOwnershipEntry_s {
	uint64_t nSourceId;
	uint32_t nPrototypeId;
	uint32_t nInstanceId;
	uint32_t nGeneration;
	t3dWorldOwnership_t eOwnership;
	t3dWorldConversionReason_t eReason;
} t3dWorldOwnershipEntry_t;

// From src/authoring/world/core/terrain_3d_world_authoring.h
typedef struct t3dWorldOwnershipRegistry_s {
	t3dWorldOwnershipEntry_t *pEntries;
	uint32_t nCapacity;
	uint32_t nCount;
	uint32_t nGeneration;
} t3dWorldOwnershipRegistry_t;

// From src/authoring/world/conversion/terrain_3d_world_godot_converter.h
typedef struct t3dWorldGodotMaterialResolver_s {
	t3dWorldResolveMaterialVTColdFn_t pResolveMaterialVT;
	void *pUser;
} t3dWorldGodotMaterialResolver_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldVertex_s {
	float m_fPosition[3];
	uint32_t m_nPackedNormal;
	uint32_t m_nPackedTangent;
	uint32_t m_nPackedUv0;
	uint32_t m_nPackedUv1;
	uint32_t m_nColorRgba8;
} t3dWorldVertex_t;

// From src/authoring/world/conversion/terrain_3d_world_godot_converter.h
typedef struct t3dWorldGodotScratch_s {
	t3dWorldVertex_s *pVertices;
	uint32_t nVertexCapacity;
	uint32_t *pIndices;
	uint32_t nIndexCapacity;
	uint32_t *pTriangleMaterialOffsets;
	uint8_t *pTriangleTiers;
	uint32_t nTriangleCapacity;
} t3dWorldGodotScratch_t;

// From src/authoring/world/conversion/terrain_3d_world_godot_converter.h
typedef struct t3dWorldPrototypeEntry_s {
	uint64_t nKey;
	uint32_t nMeshId;
	uint32_t nGeneration;
} t3dWorldPrototypeEntry_t;

// From src/authoring/world/conversion/terrain_3d_world_godot_converter.h
typedef struct t3dWorldMaterialEntry_s {
	uint64_t nKey;
	uint32_t nMaterialId;
	uint32_t nGeneration;
} t3dWorldMaterialEntry_t;

// From src/authoring/world/conversion/terrain_3d_world_godot_converter.h
typedef struct t3dWorldNativeVisualEntry_s {
	uint64_t nSourceId;
	t3dWorldOwnership_t eOwnership;
} t3dWorldNativeVisualEntry_t;

// From src/authoring/world/conversion/terrain_3d_world_godot_converter.h
typedef struct t3dWorldSceneBuildStats_s {
	uint32_t nVisitedNodes;
	uint32_t nMeshCandidates;
	uint32_t nConvertedInstances;
	uint32_t nUniquePrototypes;
	uint32_t nUniqueMaterials;
	uint32_t nCompatibilityInstances;
	uint32_t nNativeInstances;
	uint32_t nFailureCount;
} t3dWorldSceneBuildStats_t;

// From src/authoring/world/conversion/terrain_3d_world_scene_builder.h
typedef struct t3dWorldPrototypeBounds_s {
	float vCenter[3];
	float fRadius;
} t3dWorldPrototypeBounds_t;

// From src/authoring/world/conversion/terrain_3d_world_scene_builder.h
typedef struct t3dWorldCellHashEntry_s {
	int32_t iCellX;
	int32_t iCellZ;
	uint32_t nCellIndex;
	uint32_t nInstanceCount;
	float vMin[3];
	float vMax[3];
	bool bOccupied;
} t3dWorldCellHashEntry_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldTransform_s {
	float m_fRow0[4];
	float m_fRow1[4];
	float m_fRow2[4];
} t3dWorldTransform_t;

// From src/rendering/rd/world/runtime/terrain_3d_world_publication.h
typedef struct alignas( 16 ) t3dWorldInstance_s {
	uint32_t m_nClusterOffset;
	uint32_t m_nClusterCount;
	uint32_t m_nTransformIndex;
	uint32_t m_nPreviousTransformIndex;
	uint32_t m_nVisibilityLayers;
	uint32_t m_nFlags;
	uint32_t m_nGeometryKind;
	uint32_t m_nSourceId;
	uint32_t m_nMaterialOverride;
	uint32_t m_nCellId;
	float m_fLodBegin;
	float m_fLodEnd;
	float m_fFadeMargin;
	uint32_t m_nReserved0;
	uint32_t m_nReserved1;
	uint32_t m_nReserved2;
} t3dWorldInstance_t;

// From src/authoring/world/conversion/terrain_3d_world_scene_builder.h
typedef struct t3dWorldCellBuildScratch_s {
	t3dWorldInstance_s *pReorderedInstances;
	t3dWorldTransform_s *pReorderedTransforms;
	uint64_t *pReorderedSourceIds;
	uint32_t nInstanceCapacity;
	uint32_t *pInstanceCellIds;
	uint32_t *pCellCounts;
	uint32_t *pCellWriteCursor;
	uint32_t nCellScratchCapacity;
	t3dWorldCellHashEntry_t *pCellHash;
	uint32_t nCellHashCapacity;
} t3dWorldCellBuildScratch_t;

// From src/core/config/terrain_3d_startup_state.h
typedef struct t3dStartupDomainSnapshot_s {
	t3dStartupStage_t m_eStage = T3D_STARTUP_IDLE;
	uint32_t m_nGeneration = 0u;
	uint32_t m_nCompletedUnits = 0u;
	uint32_t m_nTotalUnits = 0u;
	uint64_t m_nStartUsec = 0u;
	uint64_t m_nEndUsec = 0u;
} t3dStartupDomainSnapshot_t;

// From src/core/utility/terrain_3d_util.cpp
typedef struct t3dDisplacementBuildContext_s {
	const uint8_t *m_pSource = NULL;
	uint8_t *m_pDestination = NULL;
	int m_nChannel = 0;
} t3dDisplacementBuildContext_t;

// From src/data/editing/terrain_3d_map_edit_service.h
typedef struct t3dDerivedMapUpdate_s {
	Vector2i m_location = Vector2i( INT32_MAX, INT32_MAX );
	Rect2i m_dirtyRect;
	bool m_bSuccess = false;
} t3dDerivedMapUpdate_t;

// From src/data/editing/terrain_3d_map_edit_service.h
typedef struct t3dDerivedMapTask_s {
	Vector2i m_location = Vector2i( INT32_MAX, INT32_MAX );
	Rect2i m_dirtyRect;
	size_t m_iOutputOffset = 0u;
	size_t m_nOutputBytes = 0u;
	bool m_bAvailable = false;
	bool m_bSuccess = false;
} t3dDerivedMapTask_t;

// From src/data/persistence/terrain_3d_region_persistence.h
typedef struct t3dStartupLoadStats_s {
	uint64_t m_nTotalUsecLast = 0u;
	uint64_t m_nEnumerationUsecLast = 0u;
	uint64_t m_nDeserializeUsecLast = 0u;
	uint64_t m_nStagingUsecLast = 0u;
	uint64_t m_nPublishUsecLast = 0u;
	uint64_t m_nFinalizeUsecLast = 0u;
	int m_nRegionFilesLast = 0;
	int m_nRegionsPublishedLast = 0;
	uint64_t m_nBytesReadLast = 0u;
	bool m_bSucceededLast = false;
} t3dStartupLoadStats_t;

// From src/data/runtime/terrain_3d_runtime_bake_codec.cpp
typedef struct t3dRuntimeCollisionHeightView_s {
	const uint8_t *m_pHeights = NULL;
	uint32_t m_nCount = 0u;
	int m_nWidth = 0;
} t3dRuntimeCollisionHeightView_t;

// From src/data/runtime/terrain_3d_runtime_bake_codec.h
typedef struct t3dRuntimeInstanceCellMetadata_s {
	uint16_t m_nMeshId = 0u;
	Vector2i m_cell = Vector2i( 0, 0 );
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nOrdinal = 0u;
	AABB m_originBounds;
} t3dRuntimeInstanceCellMetadata_t;

// From src/data/runtime/terrain_3d_runtime_bake_codec.h
typedef struct t3dMinMaxLevelView_s {
	const uint8_t *m_pRanges = NULL;
	uint32_t m_nRangeCount = 0u;
	uint16_t m_nSourceSide = 0u;
	uint16_t m_nLevelSide = 0u;
} t3dMinMaxLevelView_t;

// From src/data/runtime/terrain_3d_runtime_bake_codec.h
typedef struct t3dInstanceMetadataView_s {
	const uint8_t *m_pRecords = NULL;
	uint32_t m_nCellCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint16_t m_nMeshCount = 0u;
} t3dInstanceMetadataView_t;

// From src/data/runtime/terrain_3d_runtime_bake_codec.h
typedef struct t3dLightingReceiverCursor_s {
	const uint8_t *m_pData = NULL;
	size_t m_nSize = 0u;
	size_t m_iOffset = 0u;
	uint32_t m_nCount = 0u;
	uint32_t m_iIndex = 0u;
} t3dLightingReceiverCursor_t;

// From src/data/runtime/terrain_3d_runtime_page_format.h
typedef struct t3dRuntimePageChannelInfo_s {
	uint32_t m_nType = 0u;
	uint32_t m_nFormat = 0u;
	uint32_t m_nCompression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	uint32_t m_iOffset = 0u;
	uint32_t m_nCompressedBytes = 0u;
	uint32_t m_nUncompressedBytes = 0u;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
	uint32_t m_nMipCount = 0u;
	uint32_t m_nAlignment = T3D_RUNTIME_PAGE_ALIGNMENT;
	uint32_t m_nChecksum = 0u;
	uint32_t m_nSignature = 0u;
} t3dRuntimePageChannelInfo_t;

// From src/diagnostics/tests/unit_testing.cpp
typedef struct t3dRdPublicationExchangeState_s {
	uint64_t *m_pSlotGenerations;
	t3dAtomicU64_t *m_pExchangeToken;
	uint32_t *m_pWriteIndex;
	uint32_t *m_pRenderIndex;
	uint64_t *m_pRenderGeneration;
} t3dRdPublicationExchangeState_t;

// From src/diagnostics/tests/unit_testing.cpp
typedef struct t3dRdPublicationPayload_s {
	uint64_t m_nGeneration;
	uint64_t m_nCheck;
} t3dRdPublicationPayload_t;

// From src/diagnostics/tests/unit_testing.cpp
typedef struct t3dRdPublicationThreadContext_s {
	t3dRdPublicationPayload_t *m_pSlots;
	t3dAtomicU64_t *m_pToken;
	t3dAtomicBool_t *m_pProducerDone;
	t3dAtomicBool_t *m_pValid;
	t3dAtomicU64_t *m_pAcquires;
} t3dRdPublicationThreadContext_t;

// From src/eastl_config.h
typedef struct t3dEastlAllocationStats_s {
	size_t m_nLiveBytes = 0u;
	size_t m_nPeakBytes = 0u;
	size_t m_nLiveAllocations = 0u;
	size_t m_nTotalAllocations = 0u;
	size_t m_nFailedAllocations = 0u;
	size_t m_nOverflowAllocations = 0u;
	size_t m_nOverflowLiveBytes = 0u;
} t3dEastlAllocationStats_t;

// From src/hlod/build/terrain_3d_hlod_builder.cpp
typedef struct t3dHLODTriangle_s {
	Vector3 m_a;
	Vector3 m_b;
	Vector3 m_c;
	Vector3 m_center;
	AABB m_bounds;
} t3dHLODTriangle_t;

// From src/hlod/build/terrain_3d_hlod_builder.cpp
typedef struct t3dHLODBvhNode_s {
	AABB m_bounds;
	int m_nLeft = -1;
	int m_nRight = -1;
	int m_nBegin = 0;
	int m_nCount = 0;
} t3dHLODBvhNode_t;

// From src/hlod/format/terrain_3d_hlod_runtime_codec.cpp
typedef struct t3dHLODRuntimeNodeTemp_s {
	uint16_t m_nRegion;
	int64_t m_nSpawn;
	uint8_t m_nLevel;
} t3dHLODRuntimeNodeTemp_t;

// From src/hlod/format/terrain_3d_hlod_runtime_codec.h
typedef struct t3dHLODRuntimeNodePolicyRecord_s {
	int64_t m_nSpawnId = 0;
	uint8_t m_nMinLevel = 0;
} t3dHLODRuntimeNodePolicyRecord_t;

// From src/hlod/runtime/terrain_3d_hlod_streamer.h
typedef struct t3dNodePolicy_s {
	int64_t m_nSpawnId = 0;
	uint8_t m_nMinLevel = 0;
} t3dNodePolicy_t;

// From src/hlod/runtime/terrain_3d_hlod_streamer.h
typedef struct t3dHLODInstancerRepresentationState_s {
	uint8_t m_nMinimumLevel = 0;
	uint8_t m_nActiveLevel = 0;
	uint8_t m_nResidentLevel = 0;
	uint8_t m_nRetiringLevel = 0;
} t3dHLODInstancerRepresentationState_t;

// From src/instances/contracts/terrain_3d_instance_capacity.h
typedef struct t3dInstanceCellCapacityPlan_s {
	size_t m_nRequired = 0u;
	size_t m_nHeadroom = 0u;
	size_t m_nDesired = 0u;
	size_t m_nTarget = 0u;
} t3dInstanceCellCapacityPlan_t;

// From src/instances/physics/terrain_3d_instance_occlusion.h
typedef struct t3dInstanceOcclusionCell_s {
	AABB m_worldBounds;
	AABB m_localProxyBounds;
	Vector2i m_region;
	Vector2i m_cell;
	int32_t m_nMeshId = -1;
	uint32_t m_nSourceRevision = 0u;
	uint32_t m_nProxyGeneration = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint16_t m_nMirroredInstanceCount = 0u;
	uint8_t m_nProxyPolicy = 0u;
	uint8_t m_nFlags = T3D_INSTANCE_OCCLUSION_RECORD_NONE;
} t3dInstanceOcclusionCell_t;

// From src/instances/storage/terrain_3d_instance_publication_builder.h
typedef struct t3dInstancePublicationCategoryDiagnostics_s {
	uint64_t m_nPublishedLod0Cells = 0u;
	uint64_t m_nPublishedLod1Cells = 0u;
	uint64_t m_nPublishedHigherLodCells = 0u;
	uint64_t m_nLodFallbacks = 0u;
	uint64_t m_nLodMissingSourceSkips = 0u;
	uint64_t m_nVisibilityLayerFilteredCells = 0u;
	uint64_t m_nFadeActiveCells = 0u;
	uint64_t m_nHlodEligibleSources = 0u;
	uint64_t m_nHlodActiveReplacements = 0u;
	uint64_t m_nHlodDetailSuppressedCells = 0u;
	uint64_t m_nHlodDetailRetainedMissingProxy = 0u;
	uint64_t m_nHlodTransitionOverlapCells = 0u;
	uint64_t m_nHlodTransitionGapErrors = 0u;
} t3dInstancePublicationCategoryDiagnostics_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef struct t3dRenderCell_s {
	Vector2i m_region;
	Vector2i m_cell;
	int m_nMeshId = -1;
	int m_nLod = 0;
	uint32_t m_nRegionListNode = UINT32_MAX;
	int m_nUploadedCount = 0;
	uint32_t m_nUploadedRevision = 0;
	uint32_t m_nSettingsRevision = 0;
	uint32_t m_nAabbGeneration = 0;
	bool m_bUsesColors = false;
} t3dRenderCell_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef struct t3dUsableHeight_s {
	bool m_bValid = false;
	real_t m_fHeight = 0.f;
	bool m_bRaycastHit = false;
	Vector3 m_normal = Vector3( 0.f, 1.f, 0.f );
} t3dUsableHeight_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef struct t3dLogicalCellState_s {
	uint32_t m_nCount = 0;
	uint32_t m_nRegionListNode = UINT32_MAX;
} t3dLogicalCellState_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef struct t3dRdCellRange_s {
	uint32_t m_nFirst = 0u;
	uint32_t m_nCount = 0u;
} t3dRdCellRange_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef struct t3dRegionList_s {
	uint32_t m_nHead = UINT32_MAX;
	uint32_t m_nCount = 0u;
} t3dRegionList_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef struct t3dRenderRegionNode_s {
	size_t m_iRenderCell = 0u;
	uint32_t m_nNext = UINT32_MAX;
	uint32_t m_nPrev = UINT32_MAX;
} t3dRenderRegionNode_t;

// From src/instances/runtime/terrain_3d_instancer.h
typedef struct t3dTransitionBucketState_s {
	uint32_t m_nHead = UINT32_MAX;
	uint32_t m_nCount = 0u;
} t3dTransitionBucketState_t;

// From src/memory/core/terrain_3d_memory.h
typedef struct t3dRuntimeAllocationPhaseStats_s {
	uint64_t m_szEastlEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szEastlBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szZoneEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szZoneBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szRawEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szRawBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
} t3dRuntimeAllocationPhaseStats_t;

// From src/memory/policy/terrain_3d_memory_budget.h
typedef struct t3dMemorySubsystemInput_s {
	bool m_bEnabled = true;
	bool m_bAutomatic = true;
	uint64_t m_nManualBytes = 0u;
	uint64_t m_nMinimumBytes = 0u;
	uint64_t m_nHardCapBytes = UINT64_MAX;
	uint32_t m_nWeight = 1u;
} t3dMemorySubsystemInput_t;

// From src/memory/policy/terrain_3d_memory_budget.h
typedef struct t3dMemorySubsystemAllocation_s {
	uint64_t m_nRequestedBytes = 0u;
	uint64_t m_nEffectiveBytes = 0u;
	t3dMemoryLimitReason_t m_limitingReason = TERRAIN3D_MEMORY_LIMIT_NONE;
	bool m_bBelowMinimum = false;
} t3dMemorySubsystemAllocation_t;

// From src/memory/arena/terrain_3d_memory_zone.h
typedef struct t3dMemoryZoneStats_s {
	size_t m_nReservedBytes = 0u;
	size_t m_nLiveRequestedBytes = 0u;
	size_t m_nLivePhysicalBytes = 0u;
	size_t m_nLiveAllocationCount = 0u;
	size_t m_nExtentFreeBytes = 0u;
	size_t m_nSmallPageReservedBytes = 0u;
	size_t m_nSmallPageReusablePayloadBytes = 0u;
	size_t m_nSmallPageReusablePhysicalBytes = 0u;
	size_t m_nLargeAllocationPhysicalBytes = 0u;
	size_t m_nMetadataBytes = 0u;
	size_t m_nAlignmentWasteBytes = 0u;
	size_t m_nSizeClassWasteBytes = 0u;
	size_t m_nInternalFragmentationBytes = 0u;
	size_t m_nFailedAllocations = 0u;
	size_t m_nExtentTableSaturationEvents = 0u;
	size_t m_nLostFreePreventionEvents = 0u;
	uint32_t m_nActiveSmallPages = 0u;
	uint32_t m_nPeakActiveSmallPages = 0u;
	uint32_t m_nFreeExtentCount = 0u;
	uint32_t m_nPeakFreeExtentCount = 0u;
} t3dMemoryZoneStats_t;

// From src/memory/arena/terrain_3d_memory_zone.h
typedef struct t3dMemoryZoneExtent_s {
	size_t m_iOffset = 0u;
	size_t m_nSize = 0u;
} t3dMemoryZoneExtent_t;

// From src/memory/arena/terrain_3d_memory_zone.h
typedef struct t3dMemoryZoneSmallPage_s {
	size_t m_iOffset = 0u;
	uint32_t m_nBytes = 0u;
	uint32_t m_nBlockStride = 0u;
	uint32_t m_iFirstFree = UINT32_MAX;
	uint16_t m_iClass = UINT16_MAX;
	uint16_t m_nLiveBlocks = 0u;
	uint16_t m_nTotalBlocks = 0u;
	size_t m_nLiveRequestedBytes = 0u;
	bool m_bActive = false;
} t3dMemoryZoneSmallPage_t;

// From src/memory/arena/terrain_3d_memory_zone.h
typedef struct t3dMemoryZoneAllocationHeader_s {
	uint64_t m_nMagic = 0u;
	size_t m_iExtentOffset = 0u;
	size_t m_nExtentBytes = 0u;
	size_t m_nRequestedBytes = 0u;
	uint32_t m_nAlignment = 0u;
	uint16_t m_iPage = UINT16_MAX;
	uint16_t m_iClass = UINT16_MAX;
	uint64_t m_nReserved = 0u;
} t3dMemoryZoneAllocationHeader_t;

// From src/memory/storage/terrain_3d_resident_pool.h
typedef struct t3dSlotLayout_s {
	size_t m_nHeightSlotBytes = 0u;
	size_t m_nControlSlotBytes = 0u;
	size_t m_nColorSlotBytes = 0u;
	size_t m_nDerivedSlotBytes = 0u;
	size_t m_nStride = 0u;
} t3dSlotLayout_t;

// From src/rendering/rd/infrastructure/buffers/terrain_3d_local_compute_buffer_service.h
struct t3dLocalComputeBufferStats_t {
	uint64_t bufferCreates = 0u;
	uint64_t uniformSetCreates = 0u;
	uint64_t pipelineCreates = 0u;
	uint64_t resourceFailures = 0u;
	uint64_t uploadCalls = 0u;
	uint64_t bytesUploaded = 0u;
	uint64_t readbackCalls = 0u;
	uint64_t bytesReadBack = 0u;
};

// From src/rendering/rd/infrastructure/visibility/terrain_3d_rd_hiz_reducer.h
struct t3dRdHiZDispatchInfo_t {
	uint32_t m_nMip = 0u;
	uint32_t m_nSourceWidth = 0u;
	uint32_t m_nSourceHeight = 0u;
	uint32_t m_nAuxiliaryWidth = 0u;
	uint32_t m_nAuxiliaryHeight = 0u;
	uint32_t m_nDestinationWidth = 0u;
	uint32_t m_nDestinationHeight = 0u;
	bool m_bAuxiliarySourceBound = false;
};

// From src/rendering/rd/forward/terrain_3d_forward_pass_preparer.h
typedef struct t3dRdForwardViewportState_s {
		uint64_t m_nViewportKey = 0u;
		Vector2i m_renderSize;
		Rect2i m_renderRegion;
		uint64_t m_nLastRenderFrame = UINT64_MAX;
		uint64_t m_nReconfiguredRenderFrame = UINT64_MAX;
		uint32_t m_nViewCount = 0u;
		uint32_t m_nSampleCount = 0u;
		bool m_bValid = false;
	} t3dRdForwardViewportState_t;

// From src/rendering/rd/forward/terrain_3d_forward_pass_preparer.h
typedef struct t3dRdForwardDrawBaseline_s {
		uint64_t m_nSubmitCallbacks = 0u;
		uint64_t m_nTerrainDraws = 0u;
		uint64_t m_nInstanceDraws = 0u;
		uint64_t m_nOceanDraws = 0u;
		uint64_t m_nWorldDraws = 0u;
	} t3dRdForwardDrawBaseline_t;

// From src/rendering/rd/core/terrain_3d_rd_context.h
typedef struct t3dRdFreedResource_s
	{
		uint64_t m_nRid = 0u;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdFreedResource_t;

// From src/rendering/rd/core/terrain_3d_rd_context_contract.h
struct t3dRdResourceEpoch_t {
		uint64_t nCreates = 0u;
		uint64_t nFrees = 0u;
		uint64_t nLive = 0u;
		uint64_t nLiveBytes = 0u;
		uint64_t nSteadyStateViolations = 0u;
	};

// From src/rendering/rd/forward/terrain_3d_rd_forward_lighting.h
typedef struct alignas( 16 ) t3dRdForwardLightFrame_s {
		uint32_t m_nClusterShift = 0u;
		uint32_t m_nClusterWidth = 0u;
		uint32_t m_nClusterTypeSize = 0u;
		uint32_t m_nClusterMaxElementsDiv32 = 0u;
		uint32_t m_nDirectionalLightCount = 0u;
		uint32_t m_nDefaultSurfaceMask = UINT32_MAX;
		uint32_t m_nShadowFlags = 0u;
		uint32_t m_nDirectionalSoftShadows = 0u;
		float m_fCameraZNear = 0.0f;
		float m_fCameraZFar = 0.0f;
		float m_fInvCameraZFar = 0.0f;
		float m_fReflectionAtlasBorderSize = 0.0f;
		uint32_t m_nRenderWidth = 0u;
		uint32_t m_nRenderHeight = 0u;
		uint32_t m_nClusterMaxElements = 0u;
		uint32_t m_nSceneFeatureFlags = 0u;
		float m_szAmbientLightColorEnergy[4];
		float m_szEnvironmentParams0[4];
		float m_szEnvironmentParams1[4];
		float m_szRadianceInverseXform[12];
	} t3dRdForwardLightFrame_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef struct alignas( 8 ) t3dRdForwardBridgeFramePacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint64_t m_nGeneration;
	uint64_t m_nScenePass;
	uint64_t m_nRenderFrame;
	uint64_t m_nViewportKey;
	uint32_t m_nViewCount;
	uint32_t m_nSampleCount;
	uint32_t m_nCameraVisibleLayers;
	uint32_t m_nFlags;
	int32_t m_nRenderRegion[4];
	int32_t m_nRenderSize[2];
	uint32_t m_nClusterMaxElements;
	uint32_t m_nClusterShift;
	uint32_t m_nClusterWidth;
	uint32_t m_nClusterTypeSize;
	uint32_t m_nClusterMaxElementsDiv32;
	uint32_t m_nDirectionalLightCount;
	float m_fCameraZNear;
	float m_fCameraZFar;
	float m_fReflectionAtlasBorderSize;
	float m_mCameraTransform[12];
	float m_mViewProjection[T3D_FORWARD_BRIDGE_MAX_VIEWS][16];
	float m_mPreviousViewProjection[T3D_FORWARD_BRIDGE_MAX_VIEWS][16];
	float m_vEyeOffset[T3D_FORWARD_BRIDGE_MAX_VIEWS][4];
	float m_vMotionJitter[4];
	uint32_t m_nAbi10ReservedTail;
	uint32_t m_nEnvironmentFlags;
	uint32_t m_nEnvironmentReserved0;
	float m_szAmbientLightColorEnergy[4];
	float m_fAmbientColorSkyMix;
	float m_fEmissiveExposureNormalization;
	float m_fIblExposureNormalization;
	float m_fRadiancePixelSize;
	float m_fRadianceBorderSize;
	float m_fEnvironmentReserved1;
	float m_szRadianceInverseXform[12];
} t3dRdForwardBridgeFramePacket_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef struct alignas( 8 ) t3dRdForwardBridgeResourcePacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nGeneration;
	uint32_t m_nReserved0;
	uint64_t m_hDirectionalLightBuffer;
	uint64_t m_hOmniLightBuffer;
	uint64_t m_hSpotLightBuffer;
	uint64_t m_hAreaLightBuffer;
	uint64_t m_hClusterBuffer;
	uint64_t m_hShadowAtlas;
	uint64_t m_hDirectionalShadowAtlas;
	uint64_t m_hShadowSampler;
	uint64_t m_hReflectionBuffer;
	uint64_t m_hReflectionAtlas;
	uint64_t m_hDecalBuffer;
	uint64_t m_hDecalAtlas;
	uint64_t m_hDecalAtlasSrgb;
	uint64_t m_hRadianceTexture;
	uint64_t m_hPreviousDepthTexture;
} t3dRdForwardBridgeResourcePacket_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef struct alignas( 8 ) t3dRdForwardBridgePassPacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nPassKind;
	uint32_t m_nNativePassMode;
	uint32_t m_nColorFlags;
	uint32_t m_nFlags;
	int64_t m_nFramebufferFormat;
	uint64_t m_nSerial;
	uint64_t m_hNativeFramebuffer;
} t3dRdForwardBridgePassPacket_t;

// From src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h
typedef struct alignas( 8 ) t3dRdForwardBridgeShadowPacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nNativePassMode;
	uint32_t m_nFlags;
	uint32_t m_nSampleCount;
	uint32_t m_nShadowCasterMask;
	uint32_t m_nShadowLightCullMask;
	uint32_t m_nReserved0;
	int64_t m_nFramebufferFormat;
	uint64_t m_nSerial;
	uint64_t m_nScenePass;
	uint64_t m_nRenderFrame;
	uint64_t m_nViewportKey;
	float m_mCameraTransform[12];
	float m_mViewProjection[16];
	float m_vEyeOffset[4];
} t3dRdForwardBridgeShadowPacket_t;

// From src/rendering/rd/instances/terrain_3d_rd_frustum.h
typedef struct t3dRdFrustum_s {
	Vector4 m_szPlanes[6];
} t3dRdFrustum_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp
typedef struct alignas( 16 ) t3dRdInstanceFrame_s {
	float m_szViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16];
	float m_szView[16];
	float m_szPreviousViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16];
	float m_szCamera[T3D_RD_FORWARD_MAX_VIEWS][4];
	float m_szMotionJitter[4];
	float m_szMotionParams[4];
	float m_szNomadSunShape[4];
	float m_szNomadSunDiffuse[4];
	float m_szNomadSunSpecular[4];
	float m_szNomadSunRim[4];
	float m_szInstanceTextureMipBias[4]; // xyz: mip bias; w: packed category filter overrides
} t3dRdInstanceFrame_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp
typedef struct alignas( 16 ) t3dRdInstancePush_s {
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nVisibleBase = 0u;
	uint32_t m_nFlags = 0u;
	uint32_t m_nVisibilityLayers = UINT32_MAX;
	uint32_t m_nReserved0 = 0u;
	uint32_t m_nReserved1 = 0u;
	uint32_t m_nReserved2 = 0u;
} t3dRdInstancePush_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp
typedef struct t3dRdHlodCullPush_s {
	uint32_t m_nDescriptorCount = 0u;
	uint32_t m_nVisibilityMask = UINT32_MAX;
	uint32_t m_nCommandCount = 0u;
	uint32_t m_nMainCommandBase = 0u;
} t3dRdHlodCullPush_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp
typedef struct t3dRdInstanceCullPush_s {
	uint32_t m_nDescriptorBase = 0u;
	uint32_t m_nDescriptorCount = 0u;
	uint32_t m_nCommandCount = 0u;
	uint32_t m_nVisibilityMask = UINT32_MAX;
	uint32_t m_nViewCount = 1u;
	uint32_t m_nShadowPass = 0u;
	uint32_t m_nMode = 0u;
	uint32_t m_nMainCommandBase = 0u;
	uint32_t m_nShadowCommandBase = 0u;
	uint32_t m_nReserved0 = 0u;
	uint32_t m_nReserved1 = 0u;
	uint32_t m_nReserved2 = 0u;
} t3dRdInstanceCullPush_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer_contracts.h
typedef struct t3dRdInstanceDirtyRange_s {
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
} t3dRdInstanceDirtyRange_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer_contracts.h
typedef struct t3dRdInstanceDescriptorSpan_s {
	uint32_t m_nFirst = 0u;
	uint32_t m_nCount = 0u;
} t3dRdInstanceDescriptorSpan_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer_contracts.h
typedef struct t3dRdPacketBuildEntry_s {
	uint32_t m_nPacketIndex = 0u;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nCellIndex = 0u;
} t3dRdPacketBuildEntry_t;

// From src/rendering/rd/instances/terrain_3d_rd_mesh_cache.cpp
struct AttributeDesc {
	uint32_t location;
	RenderingDevice::DataFormat format;
	uint32_t offset;
};

// From src/rendering/rd/instances/terrain_3d_rd_mesh_cache.h
typedef struct t3dRdMeshSourceSurface_s {
	uint64_t m_nSourceKey = 0u;
	uint16_t m_nAssetId = 0u;
	int16_t m_nLod = 0;
	uint16_t m_nSurface = 0u;
	uint16_t m_nMaterial = UINT16_MAX;
	uint16_t m_nOverlayMaterial = UINT16_MAX;
	uint16_t m_nFlags = 0u;
	uint32_t m_nVertexOffset = 0u;
	uint32_t m_nVertexCount = 0u;
	uint32_t m_nIndexOffset = 0u;
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nWorldVertexOffset = 0u;
	uint32_t m_nWorldVertexCount = 0u;
	uint32_t m_nWorldClusterOffset = 0u;
	uint32_t m_nWorldClusterCount = 0u;
	AABB m_bounds;
	// CPU prediction metadata; never serialized or uploaded as a shader record.
	float m_szUvMin[2] = { 0.0f, 0.0f };
	float m_szUvMax[2] = { 0.0f, 0.0f };
} t3dRdMeshSourceSurface_t;

// From src/rendering/rd/instances/terrain_3d_rd_mesh_cache.h
typedef struct t3dRdMeshSourceLod_s {
	uint64_t m_nSourceKey = 0u;
	uint16_t m_nAssetId = 0u;
	int16_t m_nLod = 0;
	uint32_t m_nFirstSurface = 0u;
	uint32_t m_nSurfaceCount = 0u;
	AABB m_bounds;
} t3dRdMeshSourceLod_t;

// From src/rendering/rd/ocean/terrain_3d_rd_ocean_renderer.h
typedef struct alignas( 16 ) t3dRdOceanFrame_s {
		float m_mViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16];
		float m_mView[16];
		float m_szCameraTime[4];
		float m_szLightDirection[4];
		float m_szLightColor[4];
		float m_szMeshParams[4];
		float m_szTargetTime[4];
		float m_szWaterColor[4];
		float m_szDepthParams[4];
		float m_szWaveParams[4];
		float m_szSurfaceParams[4];
		float m_szFoamParams0[4];
		float m_szFoamParams1[4];
		float m_szScatterParams[4];
		float m_szModeParams[4];
	} t3dRdOceanFrame_t;

// From src/rendering/rd/preview/terrain_3d_rd_preview_service.h
typedef struct t3dPreviewReadbackSlot_s {
		bool bActive = false;
		bool bReadbackInFlight = false;
		uint16_t nAssetId = 0u;
		Vector2i size;
		uint64_t nRequestGeneration = 0u;
		uint64_t nLifecycleGeneration = 0u;
		uint64_t nMeshSourceGeneration = 0u;
		uint64_t nAssetObjectId = 0u;
		uint64_t nReadbackSubmitUsec = 0u;
		uint64_t nCompletionOwnerId = 0u;
		uint64_t nCompletionGeneration = 0u;
	} t3dPreviewReadbackSlot_t;

// From src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.h
typedef struct t3dRdShaderCatalogEntry_s {
	t3dRdShaderId_t m_id;
	const char *m_path;
	const char *m_version;
	t3dRdShaderDomain_t m_domain;
	t3dRdShaderCategory_t m_category;
	bool m_requiredAtStartup;
} t3dRdShaderCatalogEntry_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
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
	uint32_t m_nMicroVTAllocation = T3D_VT_INVALID_ALLOCATION;
	uint32_t m_nMicroVTGeneration = 0u;
	uint32_t m_nMicroVTFlags = 0u;
	uint32_t m_nMicroVTReserved = 0u;
} t3dRdMaterialLayer_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct alignas( 16 ) t3dRdInstanceRecord_s {
	float m_szPosition[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t m_nQuaternion01 = 0u;
	uint32_t m_nQuaternion23 = 0u;
	uint32_t m_nScale01 = 0u;
	uint32_t m_nScale2Flags = 0u;
	uint32_t m_nColorRgba8 = 0xFFFFFFFFu;
} t3dRdInstanceRecord_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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
	AABB m_occlusionBounds;
	uint32_t m_nOcclusionSourceRevision = 0u;
	uint32_t m_nOcclusionProxyGeneration = 0u;
	uint32_t m_nOcclusionProxyPolicy = 0u;
} t3dRdInstanceCell_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct alignas( 16 ) t3dRdHlodCullDescriptor_s {
	float m_szBoundsCenter[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float m_szBoundsExtent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nCommandIndex = 0u;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nReserved0 = 0u;
} t3dRdHlodCullDescriptor_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct alignas( 16 ) t3dRdInstanceCullDescriptor_s {
	float m_szBoundsCenter[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float m_szBoundsExtent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t m_nDrawSlot = UINT32_MAX;
	uint32_t m_nCommandIndex = 0u;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nFlags = 0u;
	float m_fLodBegin = 0.0f;
	float m_fLodEnd = 0.0f;
	alignas( 16 ) float m_szLocalBoundsCenter[4] = {};
	alignas( 16 ) float m_szLocalBoundsExtent[4] = {};
} t3dRdInstanceCullDescriptor_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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
} t3dRdInstanceMaterial_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct t3dRdTerrainGenerationTuple_s {
	uint64_t m_nRegionTopologyGeneration = 0u;
	uint64_t m_nPhysicalRegionMapGeneration = 0u;
	uint64_t m_nPatchGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nRegionLightingSignature = 0u;
} t3dRdTerrainGenerationTuple_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct alignas( 16 ) t3dRdFrameUniforms_s {
	float m_mViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16]{};
	float m_mView[16]{};
	float m_mPreviousViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16]{};
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
	float m_fTerrainPadding0 = 0.0f;
	float m_fTerrainPadding1 = 0.0f;
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
	float m_szCamera[T3D_RD_FORWARD_MAX_VIEWS][4]{};
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
	float m_szInstanceTextureMipBias[4]; // xyz: mip bias; w: packed category filter overrides
} t3dRdFrameUniforms_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct t3dRdDrawIndirectCommand_s {
	uint32_t m_nVertexCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nFirstVertex = 0u;
	uint32_t m_nFirstInstance = 0u;
} t3dRdDrawIndirectCommand_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct t3dRdIndexedIndirectCommand_s {
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nFirstIndex = 0u;
	int32_t m_nVertexOffset = 0;
	uint32_t m_nFirstInstance = 0u;
} t3dRdIndexedIndirectCommand_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct alignas( 16 ) t3dInstanceDrawMeta_s {
	uint32_t m_nMaterial = T3D_RD_INSTANCE_DRAW_META_INVALID_MATERIAL;
	uint32_t m_nOverlayMaterial = T3D_RD_INSTANCE_DRAW_META_INVALID_MATERIAL;
	uint32_t m_nFirstIndex = 0u;
	uint32_t m_nIndexCount = 0u;
	int32_t m_nVertexOffset = 0;
	uint32_t m_nFlags = 0u;
	uint32_t m_nVisibleBase = T3D_RD_INSTANCE_DRAW_META_DYNAMIC_VISIBLE_BASE;
	uint32_t m_nReserved = 0u;
} t3dInstanceDrawMeta_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct alignas( 16 ) t3dInstanceVTPredictMeta_s {
	uint32_t m_nBasePrediction = UINT32_MAX;
	uint32_t m_nOverlayPrediction = UINT32_MAX;
	uint32_t m_nFlags = 0u;
	uint32_t m_nReserved = 0u;
} t3dInstanceVTPredictMeta_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct t3dRdCullPushConstants_s {
	uint32_t m_nPatchCount = 0u;
	uint32_t m_nLod0PartCount = 0u;
	uint32_t m_nOuterPartCount = 0u;
	uint32_t m_nMaxPatches = T3D_MAX_CLIPMAP_LEVELS;
	uint32_t m_nStripCommandBase = 0u;
	uint32_t m_nIndexedCommandBase = 0u;
} t3dRdCullPushConstants_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct t3dRdDrawPushConstants_s {
	uint32_t m_nDirectPatchIndex = 0u;
	uint32_t m_nDirectMode = 0u;
	uint32_t m_nPadding0 = 0u;
	uint32_t m_nPadding1 = 0u;
} t3dRdDrawPushConstants_t;

// From src/rendering/rd/core/terrain_3d_render_lifecycle_state.h
typedef struct t3dRenderLifecycleStatus_s {
	uint32_t m_nStartupGeneration = 0u;
	uint32_t m_nReadyMask = T3D_RENDER_READY_NONE;
	t3dRenderLifecycleStage_t m_eStage = T3D_RENDER_LIFECYCLE_IDLE;
	uint32_t m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	uint32_t m_nSharedResourceClass = 0u;
	uint64_t m_nPreparedWorldPublicationGeneration = 0u;
	uint64_t m_nRecoveryRevision = 0u;
	uint64_t m_nRevision = 0u;
} t3dRenderLifecycleStatus_t;

// From src/rendering/rd/world/core/terrain_3d_world_render_authority.h
typedef struct t3dWorldRenderAuthorityPolicy_s {
	bool bShippingUnifiedRequired;
	bool bUnifiedWorldPass;
	bool bEditorPreviewPass;
} t3dWorldRenderAuthorityPolicy_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef struct t3dTerrainMegaVTRegion_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nFlags;
	float m_vWorldToUv[ 4 ]; // scale X/Z, bias X/Z
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nTexelsPerMeter;
	uint32_t m_nPageCount;
	uint32_t m_nReserved;
} t3dTerrainMegaVTRegion_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef struct t3dTerrainMegaVTCookRequest_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nAllocationFlags;
	int32_t m_nRegionX;
	int32_t m_nRegionZ;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nTexelsPerMeter;
	uint32_t m_nPageCount;
	uint32_t m_nCookFlags;
	uint32_t m_nPageInterior;
	uint32_t m_nPageGutter;
	uint32_t m_nPhysicalTile;
	float m_vWorldToUv[ 4 ];
} t3dTerrainMegaVTCookRequest_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef struct t3dTerrainMegaVTEditBounds_s
{
	float m_fMinWorldX;
	float m_fMinWorldZ;
	float m_fMaxWorldX;
	float m_fMaxWorldZ;
} t3dTerrainMegaVTEditBounds_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef struct t3dTerrainMegaVTEditorState_s
{
	uint32_t m_nDirtyMip0Mask;
	uint32_t m_nDirtyPageMask;
	uint32_t m_nInFlightGeneration;
	uint32_t m_nLastPublishedGeneration;
} t3dTerrainMegaVTEditorState_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef struct t3dTerrainMegaVTCookTicket_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nRegionMapIndex;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nPageMask;
	uint64_t m_nClaimId;
} t3dTerrainMegaVTCookTicket_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef struct t3dRdTerrainRegionGpu_s
{
	uint32_t m_nPackedPhysicalFlags;
	uint32_t m_nMegaVTAllocation;
	uint32_t m_nMegaVTGeneration;
	uint32_t m_nMegaVTFlags;
	float m_vMegaVTWorldToUv[ 4 ];
} t3dRdTerrainRegionGpu_t;

// From src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.h
typedef struct t3dVirtualTextureStats_s {
	uint64_t nInitializeCount;
	uint64_t nShutdownCount;
	uint64_t nCpuImageBytes;
	uint64_t nPhysicalCacheCreateEpoch;
	uint64_t nPhysicalCacheFreeEpoch;
	uint32_t nPhysicalPageSetCapacity;
	uint32_t nPhysicalCacheAllocationEvents;
	uint32_t nPhysicalCacheAllocationEventsAfterSeal;
	uint32_t nHotViolationCount;
	uint64_t nQualityDemandResetCount;
} t3dVirtualTextureStats_t;

// From src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.h
typedef struct t3dVTFinalTelemetry_s {
	uint64_t nAllocations;
	uint64_t nVirtualPages;
	uint64_t nPhysicalCapacity;
	uint64_t nPhysicalResident;
	uint64_t nPhysicalFree;
	uint64_t nPhysicalRetiring;
	uint64_t nRequestCapacity;
	uint64_t nRequestAttempts;
	uint64_t nRequestUnique;
	uint64_t nRequestSameKey;
	uint64_t nRequestHashOverflow;
	uint64_t nRequestListOverflow;
	uint64_t nRequestDropped;
	uint64_t nPageHits;
	uint64_t nPageMisses;
	uint64_t nParentFallbackHits;
	uint64_t nTailFallbackHits;
	uint64_t nResolverDepth0Samples;
	uint64_t nResolverDepth1Samples;
	uint64_t nResolverDepth2To3Samples;
	uint64_t nResolverDepth4PlusSamples;
	uint64_t nResolverTailSamples;
	uint64_t nIoJobsPending;
	uint64_t nIoBytesTotal;
	uint64_t nIoBytesPerSecond;
	uint64_t nUploadPageSetsFrame;
	uint64_t nUploadBytesFrame;
	uint64_t nUploadBytesTotal;
	uint64_t nEvictionsFrame;
	uint64_t nEvictionsTotal;
	uint64_t nThrashCount;
	uint64_t nResidentTerrain;
	uint64_t nResidentClutter;
	uint64_t nResidentGroundCover;
	uint64_t nResidentOccluder;
	uint64_t nResidentHlod;
	uint64_t nDescriptorBindsInstancerPass;
	uint64_t nDescriptorRebuildsRuntime;
	uint64_t nGpuResourceCreatesAfterSeal;
} t3dVTFinalTelemetry_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTAllocation_s {
	uint32_t m_nBaseVirtualPage;
	uint32_t m_nPageCount;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nFlags;
	uint32_t m_nTailIndex;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nReserved;
} t3dVTAllocation_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPageEntry_s {
	uint32_t m_nPhysicalSlot;
	uint32_t m_nParentPage;
	uint32_t m_nGeneration;
	uint16_t m_nResolvedMip;
	uint16_t m_nFlags;
} t3dVTPageEntry_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTMipLayout_s {
	uint32_t m_nBaseVirtualPage;
	uint16_t m_nWidthPages;
	uint16_t m_nHeightPages;
} t3dVTMipLayout_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTTailMetadata_s {
	uint32_t m_nAtlasX;
	uint32_t m_nAtlasY;
	uint16_t m_nWidthTexels;
	uint16_t m_nHeightTexels;
	uint16_t m_nFirstMip;
	uint16_t m_nMipCount;
	uint32_t m_nGeneration;
	uint32_t m_nChannelMask;
	uint32_t m_nContentId;
	uint32_t m_nFlags;
} t3dVTTailMetadata_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTResolveResult_s {
	uint32_t m_nAllocation;
	uint32_t m_nRequestedPage;
	uint32_t m_nResolvedPage;
	uint32_t m_nPhysicalSlot;
	uint32_t m_nTailIndex;
	uint16_t m_nRequestedMip;
	uint16_t m_nResolvedMip;
	uint16_t m_nFallbackDepth;
	uint16_t m_nFlags;
	uint32_t m_nReserved;
} t3dVTResolveResult_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPhysicalSlot_s {
	uint32_t m_nVirtualPage;
	uint32_t m_nGeneration;
	uint32_t m_nLastVisibleFrame;
	uint32_t m_nLastRequestedFrame;
	uint16_t m_nFlags;
	uint16_t m_nClockAge;
	uint16_t m_nPinCount;
	uint16_t m_nRetireFrame;
	uint32_t m_nPayloadId;
	uint32_t m_nReserved;
} t3dVTPhysicalSlot_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTRequest_s {
	uint32_t m_nVirtualPage;
	uint16_t m_nWantedMip;
	uint16_t m_nPriority;
} t3dVTRequest_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dRenderingMaterialVTBindings_s {
	uint32_t m_nVtAllocation;
	uint32_t m_nOverlayVtAllocation;
} t3dRenderingMaterialVTBindings_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dRenderingMaterialGpu_s {
	uint32_t m_nVtAllocation;
	uint32_t m_nOverlayVtAllocation;
	uint32_t m_nFlags;
	uint32_t m_nPackedScalars0;
	float m_vBaseColor[4];
	float m_vUvScaleBias[4];
	uint32_t m_nPackedEmission;
	uint32_t m_nPackedScalars1;
	uint32_t m_nSamplerClass;
	uint32_t m_nReserved;
} t3dRenderingMaterialGpu_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTRequestGpuCounters_s {
	uint32_t m_nCompactedRequestCount;
	uint32_t m_nRawAttemptCount;
	uint32_t m_nUniqueAcceptedCount;
	uint32_t m_nSameKeyMergeCount;
	uint32_t m_nProbeOverflowCount;
	uint32_t m_nRequestListOverflowCount;
	uint32_t m_nVisiblePredictionCount;
	uint32_t m_nFuturePredictionCount;
	uint32_t m_nMipCoarsenCount;
	uint32_t m_nBaseAllocationCount;
	uint32_t m_nOverlayAllocationCount;
	uint32_t m_nFragmentMissCount;
	uint32_t m_nFrameId;
	uint32_t m_nGeneration;
	uint32_t m_nHashCapacity;
	uint32_t m_nRequestCapacity;
} t3dVTRequestGpuCounters_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTFragmentFeedbackParams_s {
	uint32_t m_nHashCapacity;
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nFlags;
} t3dVTFragmentFeedbackParams_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTRequestHashEntry_s {
	uint32_t m_nVirtualPage;
	uint32_t m_nWantedMip;
	uint32_t m_nPriority;
	uint32_t m_nProducerMask;
} t3dVTRequestHashEntry_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPredictionMetadata_s {
	float m_vWorldSphere[4];
	float m_vUvBounds[4];
	uint32_t m_nBaseVtAllocation;
	uint32_t m_nOverlayVtAllocation;
	uint32_t m_nMaterialId;
	uint32_t m_nDrawSlot;
	float m_fTexelDensity;
	float m_fObjectScale;
	uint16_t m_nGeometryLod;
	uint8_t m_nCategory;
	int8_t m_nMipBias;
	uint32_t m_nFlags;
} t3dVTPredictionMetadata_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_contract.h
typedef struct t3dVTPredictionDispatch_s {
	uint32_t m_nHashCapacity;
	uint32_t m_nRequestCapacity;
	uint32_t m_nFrameId;
	uint32_t m_nGeneration;
	float m_vCameraLinearVelocity[3];
	float m_fPredictionHorizon;
	float m_fCameraAngularSpeed;
	float m_fViewportWidth;
	float m_fViewportHeight;
	float m_fFuturePriorityScale;
	uint32_t m_nDrawSlotCount;
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nFlags;
} t3dVTPredictionDispatch_t;

// From src/rendering/rd/vt/runtime/terrain_3d_vt_fixed_profile.h
typedef struct t3dVTFixedProfileDesc_s {
	uint32_t m_nExtent[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nSupportedChannelMask;
} t3dVTFixedProfileDesc_t;

// From src/rendering/rd/vt/gpu/terrain_3d_vt_gpu.h
typedef struct t3dVTGpuStats_s {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nDispatchCount;
	uint64_t nComputeListBarrierCount;
	uint64_t nAsyncReadbackCount;
	uint64_t nUploadBytes;
	uint64_t nReadbackConsumeCount;
	uint64_t nAcceptedRequestCount;
	uint64_t nReadbackBytes;
	uint64_t nVariableTierDispatchCount;
	uint64_t nRetireRetryCount;
	uint64_t nPageTableUpdateBytes;
	uint64_t nPageTableUpdateCalls;
	uint64_t nPageTableBatchCount;
	uint64_t nPageTableBatchEntries;
	uint64_t nRequestAttempts;
	uint64_t nRequestUnique;
	uint64_t nRequestSameKey;
	uint64_t nRequestHashOverflow;
	uint64_t nRequestListOverflow;
	uint64_t nFragmentFaultCount;
	uint32_t nReadbackRingBusyCount;
	uint32_t nReadbackRejectCount;
	uint32_t nReadbackStaleGenerationCount;
	uint32_t nReadbackInvalidPayloadCount;
	uint32_t nReadbackCadenceSkipCount;
	uint32_t nHotViolationCount;
	uint32_t nReserved;
} t3dVTGpuStats_t;

// From src/rendering/rd/vt/io/terrain_3d_vt_pack_runtime.h
typedef struct t3dVTPackRuntimeAllocator_s {
	void *pUser;
	t3dVTPackRuntimeAllocateFn_t pAllocate;
	t3dVTPackRuntimeFreeFn_t pFree;
} t3dVTPackRuntimeAllocator_t;

// From src/rendering/rd/vt/io/terrain_3d_vt_pack_runtime.h
typedef struct t3dVTQualityRuntimeStats_s {
	uint32_t nRequestedProfile;
	uint32_t nPackageFullySupportedProfileMask;
	uint32_t nFallbackAllocationCount;
	uint32_t nAllocationsByHighestProfile[T3D_VT_QUALITY_PROFILE_COUNT];
	uint32_t nAllocationsByResolvedProfile[T3D_VT_QUALITY_PROFILE_COUNT];
} t3dVTQualityRuntimeStats_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTResidencyPolicy_s {
	uint32_t nHotHoldFrames[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	uint32_t nWarmHoldFrames[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	// GPU-P01: capacity-scaled cold-page protection. Percentages deliberately
	// sum below 100 so categories can borrow unused physical cache from peers.
	uint32_t nProtectedResidentPercent[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	uint32_t nThrashWindowFrames;
	uint32_t nRetireDelayFrames;
} t3dVTResidencyPolicy_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTFileHeader_s {
	uint32_t nMagic;
	uint32_t nVersion;
	uint32_t nHeaderBytes;
	uint32_t nFlags;
	uint32_t nAllocationCount;
	uint32_t nPageCount;
	uint32_t nPayloadCount;
	uint32_t nChannelCount;
	uint64_t nAllocationOffset;
	uint64_t nPageOffset;
	uint64_t nPayloadDirectoryOffset;
	uint64_t nPayloadDataOffset;
	uint64_t nPayloadDataBytes;
	uint64_t nContentSignature;
	uint32_t nInteriorTexels;
	uint32_t nGutterTexels;
	uint32_t nPhysicalTexels;
	uint32_t nReserved0;
	uint64_t nReserved[4];
} t3dVTFileHeader_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTPageFileRecord_s {
	uint32_t nVirtualPage;
	uint32_t nParentPage;
	uint32_t nAllocationId;
	uint32_t nPayloadId;
	uint16_t nMip;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nReserved0;
	uint32_t nGeneration;
	uint32_t nReserved1;
} t3dVTPageFileRecord_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTPayloadRecord_s {
	uint64_t nOffset;
	uint32_t nCompressedBytes;
	uint32_t nDecodedBytes;
	uint32_t nCodec;
	uint32_t nChecksum;
	uint32_t nFlags;
	uint32_t nReserved;
} t3dVTPayloadRecord_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTRuntimePayloadRecord_s {
	uint64_t nOffset;
	uint32_t nBytes;
	uint32_t nChannel;
	uint32_t nContentId;
	uint32_t nChecksum;
} t3dVTRuntimePayloadRecord_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTRuntimeTailSourceRecord_s {
	uint32_t nPayloadId[T3D_VT_CHANNEL_COUNT];
	uint16_t nAtlasInteriorWidth;
	uint16_t nAtlasInteriorHeight;
} t3dVTRuntimeTailSourceRecord_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_runtime_contract.h
typedef struct t3dVTAllocationRecordV1_s {
	uint32_t nBaseVirtualPage;
	uint32_t nPageCount;
	uint32_t nMip0WidthPages;
	uint32_t nMip0HeightPages;
	uint16_t nMipCount;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nReserved0;
	uint32_t nRootPage;
	uint32_t nReserved1;
} t3dVTAllocationRecordV1_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTPackSource_s {
	const t3dVTAllocationRecordV1_s *pAllocations;
	uint32_t nAllocationCount;
	const t3dVTPageFileRecord_t *pPages;
	uint32_t nPageCount;
	const t3dVTPayloadRecord_t *pPayloads;
	uint32_t nPayloadCount;
	const uint8_t *pPayloadData;
	uint64_t nPayloadDataBytes;
} t3dVTPackSource_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTDatabaseView_s {
	const t3dVTFileHeader_t *pHeader;
	const t3dVTAllocationRecordV1_s *pAllocations;
	const t3dVTPageFileRecord_t *pPages;
	const t3dVTPayloadRecord_t *pPayloads;
	const uint8_t *pPayloadData;
	uint64_t nFileBytes;
	const t3dVTAllocation_t *pRuntimeAllocations;
	const t3dVTTailMetadata_t *pRuntimeTailMetadata;
	const t3dVTRuntimeTailSourceRecord_t *pRuntimeTailSources;
	const t3dVTRuntimePayloadRecord_t *pRuntimePayloads;
	uint32_t nRuntimePayloadCount;
	uint32_t nRuntimeTailCount;
	const t3dVTPackAllocationRecordV3_t *pV3Allocations;
	const t3dVTChannelAllocation_t *pV3ChannelAllocations;
	const t3dVTPackPageRecordV3_t *pV3Pages;
	const t3dVTPackTailRecordV3_t *pV3Tails;
	const t3dVTPackQualityViewV3_t *pV3QualityViews;
	const t3dVTPackFormatFamilyRecordV3_t *pV3FormatFamilies;
	uint32_t nV3ChannelAllocationCount;
	uint32_t nV3PageCount;
	uint32_t nV3TailCount;
	uint32_t nV3QualityViewCount;
	uint32_t nV3FormatFamilyCount;
	uint32_t nV3FullySupportedProfileMask;
	uint32_t nPackageGeneration;
	uint32_t nRuntimeKind;
	uint64_t nPackageContentSignature;
} t3dVTDatabaseView_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTRetiredPageSet_s {
	uint32_t nPhysicalSlot;
	uint32_t nVirtualPage;
	uint32_t nGeneration;
	uint32_t nReleaseFrame;
} t3dVTRetiredPageSet_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTPageSetPayloadView_s {
	const uint8_t *pChannelBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t nChannelBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t nPresentMask;
} t3dVTPageSetPayloadView_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_runtime_contract.h
typedef struct t3dVTPageTableEntry_s {
	uint32_t nPhysicalSlot;
	uint32_t nParentPage;
	uint32_t nGeneration;
	uint16_t nResolvedMip;
	uint16_t nFlags;
} t3dVTPageTableEntry_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTResidencyBuffers_s {
	t3dVTAllocation_t *pAllocations;
	uint32_t nAllocationCapacity;
	t3dVTPageTableEntry_s *pPageTable;
	uint32_t nPageTableCapacity;
	t3dVTTailMetadata_t *pTailMetadata;
	uint32_t nTailMetadataCapacity;
	t3dVTPhysicalSlot_t *pPhysicalPageSets;
	uint32_t nPhysicalPageSetCapacity;
	uint32_t *pFreeSlots;
	t3dVTRetiredPageSet_t *pRetiredPageSets;
	uint32_t *pLastEvictedFrame;
} t3dVTResidencyBuffers_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTUploadReservation_s {
	uint32_t nVirtualPage;
	uint32_t nPhysicalSlot;
	uint32_t nVictimPage;
	uint32_t nExpectedGeneration;
	uint32_t nDatabaseGeneration;
	uint16_t nWantedMip;
	uint16_t nReserved0;
} t3dVTUploadReservation_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTResidencyStats_s {
	uint64_t nTouchCount;
	uint64_t nFallbackCount;
	uint64_t nAncestorFallbackCount;
	uint64_t nTailFallbackCount;
	uint64_t nUploadReservationCount;
	uint64_t nUploadCommitCount;
	uint64_t nEvictionCount;
	uint64_t nClockScanCount;
	uint64_t nReservationFailureCount;
	uint64_t nRetireFinalizeCount;
	uint64_t nRetireDeferredCount;
	uint64_t nThrashCount;
	uint64_t nHotScanCount;
	uint64_t nWarmScanCount;
	uint64_t nColdScanCount;
	uint64_t nPinnedProtectionCount;
	uint32_t nFreeSlotCount;
	uint32_t nResidentCount;
	uint32_t nPendingSlotCount;
	uint32_t nRetiredSlotCount;
} t3dVTResidencyStats_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTPredictedPageRect_s {
	uint32_t nMip;
	uint32_t nMinPageX;
	uint32_t nMinPageY;
	uint32_t nMaxPageX;
	uint32_t nMaxPageY;
	uint32_t nPageCount;
} t3dVTPredictedPageRect_t;

// From src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h
typedef struct t3dVTGpuProfile_s {
	uint32_t nVtAllocationRecordBytes;
	uint32_t nVtPageTableEntryBytes;
	uint32_t nVtPhysicalTileSize;
} t3dVTGpuProfile_t;

// From src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h
typedef struct t3dVTBankLayout_s {
	uint32_t nBankCount;
	uint32_t nLayers[T3D_VT_MAX_TEXTURE_BANKS];
} t3dVTBankLayout_t;

// From src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h
typedef struct t3dVTGpuResourcePlan_s {
	uint64_t nBufferBytes[T3D_VT_GPU_BUFFER_COUNT];
	uint64_t nVtPhysicalBytes[T3D_VT_CHANNEL_COUNT];
	uint64_t nVtTailBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t nVtTailAtlasWidth;
	uint32_t nVtTailAtlasHeight;
	uint32_t nVtTailEntryCapacity;
	uint32_t nReserved0;
	uint64_t nTotalBufferBytes;
	uint64_t nTotalTextureBytes;
	uint64_t nTotalBytes;
} t3dVTGpuResourcePlan_t;

// From src/rendering/rd/vt/gpu/terrain_3d_vt_resources.h
typedef struct t3dVTGpuResourceStats_s {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nLiveBufferBytes;
	uint64_t nLiveTextureBytes;
	uint64_t nResourceGeneration;
	uint32_t nLiveResourceCount;
	uint32_t nHotSealViolationCount;
} t3dVTGpuResourceStats_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_runtime_contract.h
typedef struct t3dVTGpuCounters_s {
	uint32_t nRequestCount;
	uint32_t nPredictedRequestCount;
	uint32_t nDedupRequestCount;
	uint32_t nHashOverflowCount;
	uint32_t nListOverflowCount;
	uint32_t nFragmentFaultCount;
	uint32_t nReadbackIssueCount;
	uint32_t nReserved;
} t3dVTGpuCounters_t;

// From src/rendering/rd/vt/core/terrain_3d_vt_runtime_contract.h
typedef struct t3dVTRuntimeRequest_s {
	uint32_t nVirtualPage;
	uint16_t nWantedMip;
	uint16_t nPriority;
} t3dVTRuntimeRequest_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef struct t3dVTReadbackSlotHeader_s {
	uint32_t nFrameId;
	uint32_t nDatabaseGeneration;
	uint32_t nRequestCapacity;
	uint32_t nReadbackBytes;
} t3dVTReadbackSlotHeader_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef struct t3dVTStreamJob_s {
	uint64_t nReadOffset;
	uint32_t nReadBytes;
	uint32_t nExpectedDecodedBytes;
	uint32_t nVirtualPage;
	uint32_t nAllocationId;
	uint32_t nPayloadId;
	uint32_t nContentId;
	uint32_t nPageGeneration;
	uint32_t nAllocationGeneration;
	uint32_t nDatabaseGeneration;
	uint32_t nRequestFrame;
	uint32_t nDecodedBytes;
	uint32_t nScratchLane;
	uint16_t nWantedMip;
	uint16_t nPriority;
	uint16_t nPriorityBucket;
	uint16_t nChannel;
	uint16_t nState;
	uint16_t nReserved;
	uint32_t nResultCode;
} t3dVTStreamJob_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef struct t3dVTStreamingLimits_s {
	uint64_t nMaxUploadBytesPerFrame;
	uint32_t nMaxPageSetsPerFrame;
	uint32_t nMaxInFlightIoJobs;
	uint32_t nMaxAdmittedRequestsPerFrame;
} t3dVTStreamingLimits_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h
typedef struct t3dVTStreamingStats_s {
	uint64_t nQueuedCount;
	uint64_t nClaimedCount;
	uint64_t nCompletedCount;
	uint64_t nUploadedPageCount;
	uint64_t nUploadedBytes;
	uint64_t nStaleCompletionCount;
	uint64_t nStaleReadbackCount;
	uint64_t nInvalidReadbackCount;
	uint64_t nDroppedRequestCount;
	uint64_t nDuplicateMergeCount;
	uint64_t nResidentRequestCount;
	uint64_t nPriorityPromotionCount;
	uint64_t nReplacedQueuedJobCount;
	uint64_t nBudgetDeferredCount;
	uint64_t nAdmissionBudgetDeferredCount;
	uint64_t nIoBudgetDeferredCount;
	uint64_t nPageSetBudgetDeferredCount;
	uint64_t nUploadByteBudgetDeferredCount;
	uint64_t nOversizedReadCount;
	uint64_t nDecodeFailureCount;
	uint64_t nPageHitCount;
	uint64_t nPageMissCount;
	uint64_t nIoBytesTotal;
	uint64_t nAdmissionStatusCount[T3D_VT_ADMISSION_STATUS_COUNT];
	uint64_t nPriorityBucketClaimCount[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nPriorityBucketHighWater[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nQueueHighWater;
	uint32_t nActiveJobCount;
	uint32_t nInFlightIoHighWater;
} t3dVTStreamingStats_t;

// From src/rendering/rd/world/build/terrain_3d_world_cluster_cooker.h
typedef struct t3dWorldClusterCookSource_s {
	const t3dWorldVertex_s *m_pVertices;
	const uint32_t *m_pIndices;
	uint32_t m_nVertexBase;
	uint32_t m_nVertexCount;
	uint32_t m_nIndexCount;
	uint32_t m_nMaterial;
	uint32_t m_nLod;
	uint32_t m_nGeometryKind;
	uint32_t m_nClusterFlags;
	uint32_t m_nCookFlags;
	uint32_t m_nSourceId;
} t3dWorldClusterCookSource_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldCluster_s {
	float m_fCenter[3];
	float m_fRadius;
	float m_fConeAxis[3];
	float m_fConeCutoff;
	uint32_t m_nVertexIndexOffset;
	uint32_t m_nTriangleOffset;
	uint32_t m_nPackedCounts;
	uint32_t m_nPackedLodGeometryKind;
	uint32_t m_nMaterial;
	uint32_t m_nFlags;
	uint32_t m_nSourceId;
	uint32_t m_nReserved;
} t3dWorldCluster_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldClusterVTMetadata_s {
	uint32_t m_nPackedUvMin;
	uint32_t m_nPackedUvMax;
	uint32_t m_nTexelDensity;
	uint32_t m_nReserved;
} t3dWorldClusterVTMetadata_t;

// From src/rendering/rd/world/build/terrain_3d_world_cluster_cooker.h
typedef struct t3dWorldClusterCookBuffers_s {
	uint32_t *m_pClusterVertexIndices;
	uint32_t m_nClusterVertexIndexCapacity;
	uint32_t m_nClusterVertexIndexCount;
	uint32_t *m_pTriangleWords;
	uint32_t m_nTriangleWordCapacity;
	uint32_t m_nTriangleWordCount;
	t3dWorldCluster_s *m_pClusters;
	t3dWorldClusterVTMetadata_s *m_pVtMetadata;
	uint32_t m_nClusterCapacity;
	uint32_t m_nClusterCount;
} t3dWorldClusterCookBuffers_t;

// From src/rendering/rd/world/build/terrain_3d_world_cluster_cooker.h
typedef struct t3dWorldClusterCookStats_s {
	uint32_t m_nSourceTriangleCount;
	uint32_t m_nRasterTriangleCount;
	uint32_t m_nClusterCount;
	uint32_t m_nPaddedTriangleSlots;
} t3dWorldClusterCookStats_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldTerrainCluster_s {
	uint32_t m_nPatchId;
	uint32_t m_nPackedGridOrigin;
	uint32_t m_nPackedGridExtent;
	uint32_t m_nPackedVertexCountLod;
	uint32_t m_nFlags;
	float m_fOriginX;
	float m_fOriginZ;
	float m_fScale;
	uint32_t m_nMaterialContext;
	uint32_t m_nReserved0;
	uint32_t m_nReserved1;
	uint32_t m_nReserved2;
} t3dWorldTerrainCluster_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldVisibleItem_s {
	uint32_t m_nClusterOrTerrainId;
	uint32_t m_nInstanceOrPatchId;
	uint32_t m_nGeometryKind;
	uint32_t m_nFlags;
} t3dWorldVisibleItem_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldDrawIndirectArgs_s {
	uint32_t m_nVertexCount;
	uint32_t m_nInstanceCount;
	uint32_t m_nFirstVertex;
	uint32_t m_nFirstInstance;
} t3dWorldDrawIndirectArgs_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldMeshClusterSpan_s {
	uint32_t m_nVertexOffset;
	uint32_t m_nVertexCount;
	uint32_t m_nClusterOffset;
	uint32_t m_nClusterCount;
} t3dWorldMeshClusterSpan_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldFrame_s {
	float m_fViewProjection[16];
	float m_fPreviousViewProjection[16];
	float m_fFrustumPlanes[24];
	float m_fCameraAndMaxDistance[4];
	float m_fViewportAndInvSize[4];
	float m_fMotionJitter[4];
	uint32_t m_nClusterCount;
	uint32_t m_nTerrainClusterCount;
	uint32_t m_nTransformCount;
	uint32_t m_nMaterialCount;
	uint32_t m_nVisibleCapacity;
	uint32_t m_nPassKind;
	uint32_t m_nViewIndex;
	uint32_t m_nFrameId;
	uint32_t m_nPublicationGeneration;
	uint32_t m_nGeometryGeneration;
	uint32_t m_nMaterialGeneration;
	uint32_t m_nVtGeneration;
	uint32_t m_nHiZWidth;
	uint32_t m_nHiZHeight;
	uint32_t m_nHiZMipCount;
	uint32_t m_nFlags;
} t3dWorldFrame_t;

// From src/rendering/rd/world/core/terrain_3d_world_contract.h
typedef struct alignas( 16 ) t3dWorldResourceLimits_s {
	uint32_t m_nMaxSourceVertices;
	uint32_t m_nMaxClusters;
	uint32_t m_nMaxTerrainClusters;
	uint32_t m_nMaxTransforms;
	uint32_t m_nMaxVisibleItems;
	uint32_t m_nMaxMaterials;
	uint32_t m_nMaxClusterVertexIndices;
	uint32_t m_nMaxTriangleSlots;
	uint64_t m_nGeometryHeapBytes;
	uint64_t m_nTriangleHeapBytes;
	uint64_t m_nVisibilityBytes;
	uint64_t m_nWorldUploadBytesPerFrame;
	uint64_t m_nPublicationScratchBytes;
	uint32_t m_nHiZMaxWidth;
	uint32_t m_nHiZMaxHeight;
	uint32_t m_nHiZMaxMipCount;
	uint32_t m_nVtPageRequestBudget;
	uint32_t m_nVtPageAdmissionBudget;
	uint32_t m_nReserved0;
} t3dWorldResourceLimits_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.cpp
typedef struct t3dWorldGeometryRequirements_s {
	uint64_t nSourceTriangleCount;
	uint64_t nClusterVertexReferenceCount;
	uint64_t nTriangleSlotCount;
	uint32_t nClusterCount;
	uint32_t nMinimumClusterTriangleCount;
	uint32_t nMaximumClusterTriangleCount;
} t3dWorldGeometryRequirements_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldPackedClusterTriangle_s {
	uint32_t nLocalIndices;
} t3dWorldPackedClusterTriangle_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldMeshRecord_s {
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nMaterialBase;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
} t3dWorldMeshRecord_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldMeshLodRecord_s {
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	float fMinDistance;
	float fMaxDistance;
	float fScreenError;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
} t3dWorldMeshLodRecord_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldClusterRecord_s {
	float vCenter[3];
	float fRadius;
	float vConeAxis[3];
	float fConeCutoff;
	uint32_t nVertexIndexOffset;
	uint32_t nTriangleOffset;
	uint16_t nVertexCount;
	uint16_t nTriangleCount;
	uint16_t nLod;
	uint16_t nTier;
	uint32_t nMaterialId;
	uint32_t nPackedUvMin;
	uint32_t nPackedUvMax;
	uint32_t nTexelDensity;
} t3dWorldClusterRecord_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldGeometrySourceLod_s {
	const uint32_t *pIndices;
	const uint32_t *pTriangleMaterialOffsets;
	const uint8_t *pTriangleTiers;
	uint32_t nIndexCount;
	uint32_t nDefaultTier;
	float fMinDistance;
	float fMaxDistance;
	float fScreenError;
	uint32_t nFlags;
} t3dWorldGeometrySourceLod_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldGeometrySourceMesh_s {
	const t3dWorldVertex_t *pVertices;
	const t3dWorldGeometrySourceLod_t *pLods;
	uint32_t nVertexCount;
	uint32_t nLodCount;
	uint32_t nMaterialBase;
	uint32_t nFlags;
} t3dWorldGeometrySourceMesh_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldGeometryDatabaseBuffers_s {
	t3dWorldVertex_t *pVertexHeap;
	uint32_t nVertexCapacity;
	uint32_t *pClusterVertexIndexHeap;
	uint32_t nClusterVertexIndexCapacity;
	t3dWorldPackedClusterTriangle_t *pTriangleHeap;
	uint32_t nTriangleCapacity;
	t3dWorldMeshRecord_t *pMeshRecords;
	uint32_t nMeshCapacity;
	t3dWorldMeshLodRecord_t *pLodRecords;
	uint32_t nLodCapacity;
	t3dWorldClusterRecord_t *pClusterRecords;
	t3dWorldCluster_t *pWorldClusterRecords;
	t3dWorldClusterVTMetadata_t *pWorldClusterVtMetadata;
	uint32_t nClusterCapacity;
} t3dWorldGeometryDatabaseBuffers_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldGeometryCookedMeshSpan_s {
	uint32_t nMeshId;
	uint32_t nVertexOffset;
	uint32_t nVertexCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nReserved;
} t3dWorldGeometryCookedMeshSpan_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldGeometryCookStats_s {
	uint64_t nSourceTriangleCount;
	uint64_t nClusterTriangleSlotCount;
	uint64_t nClusterVertexReferenceCount;
	uint32_t nClusterCount;
	uint32_t nMinimumClusterTriangleCount;
	uint32_t nMaximumClusterTriangleCount;
	uint32_t nReserved;
} t3dWorldGeometryCookStats_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry_internal.h
typedef struct t3dWorldGeometryDerivedCluster_s {
	float vCenter[3];
	float fRadius;
	float vConeAxis[3];
	float fConeCutoff;
	uint32_t nPackedUvMin;
	uint32_t nPackedUvMax;
	uint32_t nTexelDensity;
} t3dWorldGeometryDerivedCluster_t;

// From src/rendering/rd/world/visibility/terrain_3d_world_hiz.h
typedef struct t3dWorldHiZReducePush_s {
		uint32_t m_nSourceWidth;
		uint32_t m_nSourceHeight;
		uint32_t m_nDestinationWidth;
		uint32_t m_nDestinationHeight;
	} t3dWorldHiZReducePush_t;

// From src/rendering/rd/world/runtime/terrain_3d_world_publication.h
typedef struct alignas( 16 ) t3dWorldCell_s {
	float m_fCenter[3];
	float m_fRadius;
	uint32_t m_nFirstInstance;
	uint32_t m_nInstanceCount;
	uint32_t m_nVisibilityLayers;
	uint32_t m_nFlags;
} t3dWorldCell_t;

// From src/rendering/rd/world/runtime/terrain_3d_world_publication.h
typedef struct alignas( 16 ) t3dWorldHlodRecord_s {
	uint32_t m_nFirstInstance;
	uint32_t m_nInstanceCount;
	uint32_t m_nClusterOffset;
	uint32_t m_nClusterCount;
	uint32_t m_nProxySourceId;
	uint32_t m_nDetailSourceId;
	uint32_t m_nFlags;
	uint32_t m_nReserved;
} t3dWorldHlodRecord_t;

// From src/rendering/rd/world/runtime/terrain_3d_world_publication.h
typedef struct alignas( 16 ) t3dWorldTerrainClusterBounds_s {
	float m_fCenter[3];
	float m_fRadius;
} t3dWorldTerrainClusterBounds_t;

// From src/rendering/rd/world/runtime/terrain_3d_world_publication.h
typedef struct t3dWorldPublicationCapacity_s {
	size_t m_nVertices;
	size_t m_nClusterVertexIndices;
	size_t m_nTriangleWords;
	size_t m_nClusters;
	size_t m_nClusterVtMetadata;
	size_t m_nTerrainStripClusters;
	size_t m_nTerrainStripClusterBounds;
	size_t m_nTransforms;
	size_t m_nInstances;
	size_t m_nCells;
	size_t m_nHlodRecords;
	size_t m_nMaterials;
	size_t m_nVisibilityCandidates;
} t3dWorldPublicationCapacity_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.h
typedef struct t3dWorldGeometryDatabase_s {
	t3dWorldGeometryDatabaseBuffers_t Buffers;
	uint32_t nVertexCount;
	uint32_t nClusterVertexIndexCount;
	uint32_t nTriangleCount;
	uint32_t nMeshCount;
	uint32_t nLodCount;
	uint32_t nClusterCount;
	uint64_t nContentSignature;
} t3dWorldGeometryDatabase_t;

// From src/rendering/rd/world/build/terrain_3d_world_static_builder.h
struct t3dWorldStaticBuildSource_t {
	const t3dWorldGeometryDatabase_s *m_pGeometry = NULL;
	const t3dWorldTransform_t *m_pTransforms = NULL;
	const t3dWorldInstance_t *m_pInstances = NULL;
	const t3dWorldCell_t *m_pCells = NULL;
	const t3dRenderingMaterialGpu_t *m_pMaterials = NULL;
	uint32_t m_nTransformCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nCellCount = 0u;
	uint32_t m_nMaterialCount = 0u;
};

// From src/rendering/rd/world/core/terrain_3d_world_system_limits.h
typedef struct t3dWorldLimits_s {
	uint32_t nMaxCells;
	uint32_t nMaxInstances;
	uint32_t nMaxMeshes;
	uint32_t nMaxMeshLods;
	uint32_t nMaxClusters;
	uint32_t nMaxMaterials;
	uint32_t nMaxVisibleCells;
	uint32_t nMaxVisibleInstances;
	uint32_t nMaxVisibleClusters[T3D_WORLD_TIER_COUNT];
	uint32_t nMaxVtVirtualPages;
	uint32_t nMaxVtPhysicalPageSets;
	uint32_t nMaxVtRequests;
	uint32_t nMaxStreamJobs;
	uint32_t nStreamScratchLaneCount;
	uint64_t nMaxStreamPayloadBytes;
	uint64_t nMaxUploadBytesPerFrame;
	uint64_t nMaxReadbackBytesPerFrame;
	uint64_t nGeometryVertexHeapBytes;
	uint64_t nGeometryClusterVertexIndexHeapBytes;
	uint64_t nGeometryTriangleHeapBytes;
	uint64_t nSnapshotBytesPerFrame;
	uint64_t nFrameScratchBytesPerFrame;
	uint64_t nStreamScratchBytesPerLane;
	uint32_t nOperationEventCapacity;
	uint32_t nReserved;
} t3dWorldLimits_t;

// From src/rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.h
typedef struct t3dVisibilityPush_s {
		uint32_t m_nCandidateCount;
		uint32_t m_nVisibilityMask;
		uint32_t m_nFlags;
		uint32_t m_nPassKind;
		uint32_t m_nTerrainBucketCounts[T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT];
		uint32_t m_nReserved0;
		uint32_t m_nReserved1;
	} t3dVisibilityPush_t;

// From src/rendering/geometry/terrain_3d_clipmap_bounds.h
typedef struct t3dClipmapVerticalBounds_s {
	double m_fMinY = -0.1;
	double m_fSizeY = 0.2;
} t3dClipmapVerticalBounds_t;

// From src/rendering/geometry/terrain_3d_clipmap_strip_contract.h
typedef struct t3dClipmapStripPart_s {
	int16_t m_nOriginX;
	int16_t m_nOriginZ;
	uint16_t m_nCellWidth;
	uint16_t m_nCellHeight;
	uint16_t m_nPartCode;
	uint16_t m_nFlags;
} t3dClipmapStripPart_t;

// From src/rendering/compute/terrain_3d_compute.cpp
typedef struct alignas( 16 ) t3dMaterialData_s {
		float m_fUvScale;
		float m_fOffset;
		float m_fScale;
		float m_fPad;
	} t3dMaterialData_t;

// From src/rendering/compute/terrain_3d_compute.cpp
typedef struct t3dDisplacementParams_s {
		uint32_t m_nWidth, m_nHeight;
		float m_fVertexSpacing, m_fVertexDensity, m_fMeshSize, m_fTessellationLevel;
		float m_fTargetX, m_fTargetZ, m_fBlendSharpness, m_fDisplacementSharpness;
		float m_fAutoSlope, m_fAutoHeightReduction;
		uint32_t m_nAutoBaseTexture, m_nAutoOverlayTexture, m_bAutoEnabled, m_nSamplingMode;
		uint32_t m_bFastBlendCurve, m_nBackgroundMode;
		float m_fGroundLevel, m_fRegionBlend;
		float m_fHeightMin, m_fHeightRange;
		uint32_t m_bControlMapsImplicit, m_nTextureLayerCount, m_nPadding1, m_nPadding2;
	} t3dDisplacementParams_t;

// From src/rendering/compute/terrain_3d_compute.cpp
typedef struct t3dDerivedParams_s {
		uint32_t m_nWidth;
		uint32_t m_nHeight;
		uint32_t m_nHaloStride;
		float m_fVertexSpacing;
	} t3dDerivedParams_t;

// From src/rendering/compute/terrain_3d_compute.h
typedef struct t3dBrushElement_s {
	uint32_t m_nValue = 0u;  // Height float bits, packed pControl, or RGBA8.
	uint32_t m_nAlpha = 0u;  // Float bits.
	uint32_t m_nTarget = 0u; // Gradient/height m_nTarget float bits.
	uint32_t m_nFlags = 0u;  // Bit 0: eligible.
} t3dBrushElement_t;

// From src/rendering/compute/terrain_3d_compute.h
typedef struct t3dBrushParams_s {
	uint32_t m_nMapType = 0u;
	uint32_t m_nTool = 0u;
	uint32_t m_nOperation = 0u;
	uint32_t m_nCount = 0u;
	float m_fStrength = 0.0f;
	float m_fSelectedHeight = 0.0f;
	float m_fBrushCenterHeight = 0.0f;
	float m_fRoughness = 0.0f;
	uint32_t m_nColorRgba8 = 0xffffffffu;
	uint32_t m_nAssetId = 0u;
	uint32_t m_nUvRotation = 0u;
	uint32_t m_nUvScale = 0u;
	uint32_t m_nFeatureFlags = 0u;
} t3dBrushParams_t;

// From src/rendering/compute/terrain_3d_compute.h
typedef struct t3dDerivedMapComputeJob_s {
	const float *m_pHeightHalo = NULL;
	uint8_t *m_pRgbaOutput = NULL;
	uint32_t m_nHaloElements = 0u;
	uint32_t m_nOutputBytes = 0u;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
	float m_fVertexSpacing = 1.0f;
	bool m_bSuccess = false;
} t3dDerivedMapComputeJob_t;

// From src/rendering/surface/terrain_3d_far_surface_atlas_cache.cpp
typedef struct t3dFarSurfaceBakeData_s {
	uint8_t m_populated[T3D_MAX_TEXTURE_ASSETS] = {};
	int m_nCount = 0;
	int m_nFallbackTexture = 0;
} t3dFarSurfaceBakeData_t;

// From src/rendering/surface/terrain_3d_far_surface_atlas_cache.h
typedef struct t3dFarSurfaceAtlasStats_s {
	int m_nHeightUpdatesLastTick = 0;
	int m_nColorUpdatesLastTick = 0;
	int m_nHeightRegionsLastTick = 0;
	int m_nColorRegionsLastTick = 0;
	int m_nHeightDirtyRegions = 0;
	int m_nColorDirtyRegions = 0;
	uint64_t m_nHeightProcessingUsecLast = 0u;
	uint64_t m_nColorProcessingUsecLast = 0u;
	uint64_t m_nHeightDirtyTexelsLast = 0u;
	uint64_t m_nColorDirtyTexelsLast = 0u;
	uint64_t m_nHeightRebuiltBytesLast = 0u;
	uint64_t m_nColorRebuiltBytesLast = 0u;
	uint64_t m_nHeightUploadedBytesLast = 0u;
	uint64_t m_nColorUploadedBytesLast = 0u;
	uint64_t m_nHeightPublicationUsecLast = 0u;
	uint64_t m_nColorPublicationUsecLast = 0u;
	uint64_t m_nHeightPublicationCount = 0u;
	uint64_t m_nColorPublicationCount = 0u;
	uint64_t m_nHeightUploadedBytesTotal = 0u;
	uint64_t m_nColorUploadedBytesTotal = 0u;
	uint64_t m_nCookIoUsecLast = 0u;
	bool m_bHeightDirtyAll = true;
	bool m_bColorDirtyAll = true;
	bool m_bCookedLoaded = false;
	bool m_bCookedSaved = false;
} t3dFarSurfaceAtlasStats_t;

// From src/rendering/material/terrain_3d_material.h
typedef struct t3dShaderVariantState_s {
	t3dWorldBackground_t m_worldBackground = FLAT;
	t3dTextureFiltering_t m_textureFiltering = LINEAR;
	t3dTerrainSamplingMode_t m_terrainSamplingMode = TERRAIN_SAMPLING_ADAPTIVE;
	t3dTerrainNormalMode_t m_terrainNormalMode = TERRAIN_NORMAL_ADAPTIVE;
	t3dColorSamplingMode_t m_colorSamplingMode = COLOR_SAMPLING_AUTO;
	t3dProjectionMode_t m_projectionMode = PROJECTION_DISABLED;
	t3dTessellationMaterialMode_t m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
	t3dSurfaceDataMode_t m_surfaceDataMode = SURFACE_DATA_AUTO;
	t3dTextureSurfaceMode_t m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	t3dTerrainShadingMode_t m_terrainShadingMode = SHADING_LAMBERT_GGX;
	t3dDetilingMode_t m_detilingMode = DETILING_CONTINUOUS;
	t3dWorldNoiseShaderMode_t m_worldNoiseShaderMode = WORLD_NOISE_DYNAMIC;
	bool m_bAutoShaderEnabled = false;
	bool m_bDualScalingEnabled = false;
	bool m_bMacroVariationEnabled = false;
	bool m_bOutputAlbedoEnabled = true;
	bool m_bOutputRoughnessEnabled = true;
	bool m_bOutputSpecularEnabled = true;
	bool m_bOutputNormalMapEnabled = true;
	bool m_bOutputAmbientOcclusionEnabled = true;
	bool m_bFastBlendCurve = false;
	bool m_bTessellationEnabled = false;
	bool m_bSurfacePacked = false;
	bool m_bControlMapsImplicit = false;
	bool m_bColorMapsImplicit = false;
} t3dShaderVariantState_t;

// From src/rendering/material/terrain_3d_material.h
typedef struct t3dTierQualityConfig_s {
	t3dTextureFiltering_t m_textureFiltering = LINEAR;
	t3dMaterialSamplingQuality_t m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
	t3dTerrainNormalQuality_t m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
	t3dTerrainColorQuality_t m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
	t3dSurfaceResponseQuality_t m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
	t3dTextureSurfaceMode_t m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	t3dTierLightingModel_t m_lightingModel = TIER_LIGHTING_LAMBERT;
	t3dTierDetilingQuality_t m_detilingQuality = TIER_DETILING_DISABLED;
	real_t m_fAuthoredColorContribution = 1.0f;
	real_t m_fWetnessContribution = 1.0f;
	real_t m_fMaterialLodBias = 0.0f;
} t3dTierQualityConfig_t;

// From src/rendering/geometry/terrain_3d_mesher.h
typedef struct t3dMesherRuntimeConfig_s {
	int m_nNearShaderLodCount = T3D_MAX_CLIPMAP_LEVELS;
	int m_nMiddleShaderLodCount = 0;
	int m_nImmediateLodCount = T3D_MAX_CLIPMAP_LEVELS;
	int m_nClipmapUpdateBudget = T3D_MAX_CLIPMAP_LEVELS;
	int m_nShadowLodCount = 0;
	int m_szTierShadowLodCounts[3] = { -1, -1, -1 };
	bool m_szTierShadowEnabled[3] = { true, true, true };
} t3dMesherRuntimeConfig_t;

// From src/rendering/surface/terrain_3d_ocean_settings.h
typedef struct t3dOceanSettingsState_s {
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
} t3dOceanSettingsState_t;

// From src/rendering/material/terrain_3d_shader_specializer.h
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

// From src/simd/core/terrain_3d_parallel.h
typedef struct t3dParallelTileDispatchContext_s {
	size_t m_nWidth = 0u;
	size_t m_nHeight = 0u;
	size_t m_nTileWidth = 1u;
	size_t m_nTileHeight = 1u;
	size_t m_nTilesX = 0u;
	t3dParallelTileTask_t m_task = NULL;
	void *m_pContext = NULL;
} t3dParallelTileDispatchContext_t;

// From src/simd/core/terrain_3d_simd.h
typedef struct t3dSimdCapabilities_s {
	bool m_bSse2 = false;
	bool m_bAvx = false;
	bool m_bF16c = false;
	bool m_bAvx2 = false;
	bool m_bNeon = false;
} t3dSimdCapabilities_t;

// From src/streaming/residency/terrain_3d_active_height_residency.h
typedef struct t3dActiveHeightResidencyStats_s {
	int m_nReconciliationsLastTick = 0;
	int m_nSlotsExaminedLastTick = 0;
	int m_nRequiredLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nPublishedLastTick = 0;
	int m_nEvictedLastTick = 0;
	int m_nPredictedRequestsLastTick = 0;
} t3dActiveHeightResidencyStats_t;

// From src/streaming/policy/terrain_3d_adaptive_budget_policy.h
typedef struct t3dAdaptiveBudgetValues_s {
	int m_nGpuPages = 4;
	int m_nCpuPages = 4;
	int m_nInstancePages = 4;
	int m_nInstancerCells = 32;
	int m_nNodeLoadRegions = 2;
	int m_nNodeUnloadRegions = 4;
	int m_nWorldUploadKb = 4096;
	int m_nWorldClusters = 8192;
	int m_nVtRequests = 2048;
	int m_nVtAdmissions = 64;
} t3dAdaptiveBudgetValues_t;

// From src/streaming/policy/terrain_3d_adaptive_budget_policy.h
typedef struct t3dAdaptiveBudgetPressure_s {
	bool m_bGpuLimited = false;
	bool m_bCpuLimited = false;
	bool m_bInstanceLimited = false;
	bool m_bInstancerLimited = false;
	bool m_bNodeLoadLimited = false;
	bool m_bNodeUnloadLimited = false;
	bool m_bWorldLimited = false;
	bool m_bVtLimited = false;
} t3dAdaptiveBudgetPressure_t;

// From src/streaming/cache/terrain_3d_cpu_region_cache.h
typedef struct t3dCpuRegionCacheStats_s {
	bool m_bEnabled = false;
	bool m_bSelectionPending = true;
	int m_nCapacity = 0;
	int m_nResident = 0;
	int m_nDesiredLastTick = 0;
	int m_nLoadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	int m_nPinnedLastTick = 0;
	uint32_t m_nStallFlagsLastTick = 0u;
	size_t m_nLoadBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
} t3dCpuRegionCacheStats_t;

// From src/streaming/policy/terrain_3d_gpu_cache_policy.h
typedef struct t3dGpuCacheCoord_s {
	int m_nX;
	int m_nY;
} t3dGpuCacheCoord_t;

// From src/streaming/policy/terrain_3d_gpu_cache_policy.h
typedef struct t3dGpuCacheSelectionStats_s {
	int m_nRequired = 0;
	int m_nGuard = 0;
	int m_nPrefetch = 0;
	int m_nWritten = 0;
} t3dGpuCacheSelectionStats_t;

// From src/streaming/cache/terrain_3d_gpu_region_cache.h
typedef struct t3dGpuRegionCacheStats_s {
	bool m_bEnabled = false;
	bool m_bTexturesReady = false;
	bool m_bSelectionPending = true;
	bool m_bCameraMovedLastTick = false;
	uint64_t m_nBudgetBytes = 0u;
	int m_nBudgetMb = 0;
	int m_nCapacity = 0;
	int m_nPhysicalLayers = 0;
	int m_nSpareLayers = 0;
	int m_nResident = 0;
	int m_nRetired = 0;
	int m_nDesiredLastTick = 0;
	int m_nRequiredLastTick = 0;
	int m_nGuardLastTick = 0;
	int m_nPrefetchLastTick = 0;
	int m_nHysteresisLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nRequestedPages = 0;
	int m_nRequiredRadius = 0;
	int m_nUploadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	int m_nCookedHitsLastTick = 0;
	int m_nCookedMissesLastTick = 0;
	int m_nRuntimePreparesLastTick = 0;
	int m_nReusedLayersLastTick = 0;
	int m_nPageTableUpdatesLastTick = 0;
	int m_nHeightUpdatesLastTick = 0;
	int m_nControlUpdatesLastTick = 0;
	int m_nColorUpdatesLastTick = 0;
	int m_nDerivedUpdatesLastTick = 0;
	int m_nMiddleCompositeUpdatesLastTick = 0;
	int m_nTerrainShadowUpdatesLastTick = 0;
	int m_nRuntimeColorMipsLastTick = 0;
	int m_nRuntimeDerivedBuildsLastTick = 0;
	int m_nPreparedPagesLastTick = 0;
	int m_nPrepareWorkersLastTick = 0;
	int m_nEffectiveUploadLimitLastTick = 0;
	uint32_t m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	size_t m_nUploadBytesLastTick = 0u;
	size_t m_nHeightBytesLastTick = 0u;
	size_t m_nControlBytesLastTick = 0u;
	size_t m_nColorBytesLastTick = 0u;
	size_t m_nDerivedBytesLastTick = 0u;
	size_t m_nMiddleCompositeBytesLastTick = 0u;
	size_t m_nTerrainShadowBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	uint64_t m_nPreparePublicationUsecLast = 0u;
	uint64_t m_nPrepareEndToEndUsecLast = 0u;
} t3dGpuRegionCacheStats_t;

// From src/streaming/cache/terrain_3d_gpu_region_cache.h
typedef struct t3dGpuRegionCacheUpdateResult_s {
	bool m_bPageTableChanged = false;
} t3dGpuRegionCacheUpdateResult_t;

// From src/streaming/cache/terrain_3d_middle_height_region_cache.h
typedef struct t3dMiddleHeightRegionCacheConfig_s {
	bool m_bAuthoring = false;
	int m_nHotRadius = 0;
	int m_nResolutionDivisor = 1;
	int m_nRetireFrames = T3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
} t3dMiddleHeightRegionCacheConfig_t;

// From src/streaming/cache/terrain_3d_middle_height_region_cache.h
typedef struct t3dMiddleHeightRegionCacheStats_s {
	int m_nResident = 0;
	int m_nMappedCapacity = 0;
	int m_nPhysicalLayers = 0;
	int m_nDesiredLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nUploadsLastTick = 0;
	int m_nEvictionsLastTick = 0;
	int m_nResolutionDivisor = 1;
	size_t m_nReservedBytes = 0u;
	bool m_bPageTableValid = false;
	bool m_bHeightArrayValid = false;
} t3dMiddleHeightRegionCacheStats_t;

// From src/streaming/runtime/terrain_3d_runtime_page_store.h
typedef struct t3dRuntimePageNumericConfig_s {
	int m_nColorDivisor = 1;
	int m_nDerivedDivisor = 1;
	int m_nMiddleHeightDivisor = 1;
	int m_nPrepareThreads = 2;
} t3dRuntimePageNumericConfig_t;

// From src/streaming/runtime/terrain_3d_runtime_page_store.h
typedef struct t3dRuntimePageProbe_s {
	t3dRuntimePageProbeState_t m_state = RUNTIME_PAGE_PROBE_MISSING;
	uint32_t m_nVersion = 0u;
	uint32_t m_nHeaderBytes = 0u;
	uint32_t m_nTotalBytes = 0u;
	uint32_t m_nChannelCount = 0u;
	uint64_t m_nFileBytes = 0u;
} t3dRuntimePageProbe_t;

// From src/streaming/runtime/terrain_3d_runtime_page_store.h
typedef struct t3dRuntimePageStats_s {
	int m_nPreparedPages = 0;
	int m_nPreparedLoadedPages = 0;
	int m_nPendingPages = 0;
	int m_nQueuedPages = 0;
	int m_nPrepareWorkers = 0;
	bool m_bPrepareJobActive = false;
	uint64_t m_nPrepareLatencyUsecLast = 0u;
	uint64_t m_nPrepareQueueWaitUsecLast = 0u;
	uint64_t m_nPrepareFileOpenUsecLast = 0u;
	uint64_t m_nPrepareFileReadUsecLast = 0u;
	uint64_t m_nPrepareValidationUsecLast = 0u;
	uint64_t m_nPrepareMaterializeUsecLast = 0u;
	uint64_t m_nPrepareWorkerUsecLast = 0u;
	uint64_t m_nPrepareReadyWaitUsecLast = 0u;
	uint64_t m_nPreparedByteBudget = 0u;
	uint64_t m_nPreparedReservedBytes = 0u;
	uint64_t m_nPayloadBytesPerSlot = 0u;
	uint64_t m_nWorkerScratchReservedBytes = 0u;
	int m_nConfiguredPrepareWorkers = 0;
	int m_nEffectivePrepareWorkers = 0;
	bool m_bCapacityFailureLatched = false;
} t3dRuntimePageStats_t;

// From src/streaming/runtime/terrain_3d_runtime_page_store.h
typedef struct t3dRuntimePageMeta_s {
	Vector2i m_location = Vector2i( INT32_MAX, INT32_MAX );
	int16_t m_iSourceIndex = -1;
	int16_t m_iPayloadSlot = -1;
	uint32_t m_nExpectedChannelFlags = 0u;
	uint64_t m_nExpectedSourceSignature = 0u;
	int m_nColorDivisor = 1;
	int m_nDerivedDivisor = 1;
	int m_nMiddleHeightDivisor = 1;
	uint32_t m_nMaterialMask = 1u;
	uint16_t m_nConsumerMask = 0u;
	uint16_t m_nAttemptedConsumerMask = 0u;
	uint16_t m_nMaterializedConsumerMask = 0u;
	uint16_t m_nLoadedConsumerMask = 0u;
	uint64_t m_nRequestedUsec = 0u;
	uint64_t m_nWorkerStartedUsec = 0u;
	uint64_t m_nFileOpenUsec = 0u;
	uint64_t m_nFileReadUsec = 0u;
	uint64_t m_nValidationUsec = 0u;
	uint64_t m_nMaterializeUsec = 0u;
	uint64_t m_nWorkerFinishedUsec = 0u;
	bool m_bAttempted = false;
	bool m_bLoaded = false;
	bool m_bInstanceAttempted = false;
	bool m_bInstanceLoaded = false;
	bool m_bLoadMinMax = false;
	bool m_bEditorAuthoring = false;
} t3dRuntimePageMeta_t;

// From src/streaming/policy/terrain_3d_runtime_profile.h
typedef struct t3dRuntimeProfileSample_s {
	uint64_t m_nFrameSequence = 0u;
	uint64_t m_nPhysicsTotalUsec = 0u;
	uint64_t m_nDataResidencyUsec = 0u;
	uint64_t m_nHlodStreamingUsec = 0u;
	uint64_t m_nNodeStreamingUsec = 0u;
	uint64_t m_nClipmapUsec = 0u;
	uint64_t m_nOceanUsec = 0u;
	uint64_t m_nInstancerUsec = 0u;
	uint64_t m_nLightSyncUsec = 0u;
	uint64_t m_nCollisionUsec = 0u;
	uint64_t m_nStreamingUsec = 0u;
	float m_fTargetX = 0.0f;
	float m_fTargetZ = 0.0f;
	float m_fMovementX = 0.0f;
	float m_fMovementZ = 0.0f;
	int m_nTargetRegionX = 0;
	int m_nTargetRegionZ = 0;
	uint32_t m_nEventFlags = TERRAIN3D_PROFILE_EVENT_NONE;
	uint32_t m_nStallFlags = TERRAIN3D_PROFILE_STALL_NONE;
	int m_nGpuResident = 0;
	int m_nGpuCapacity = 0;
	int m_nGpuUploads = 0;
	int m_nGpuEvictions = 0;
	int m_nGpuMissingRequired = 0;
	uint64_t m_nGpuUploadBytes = 0u;
	int m_nGpuPreparedPages = 0;
	int m_nGpuPrepareWorkers = 0;
	int m_nGpuEffectiveUploadLimit = 0;
	int m_nGpuCameraMoved = 0;
	int m_nGpuRequestedPages = 0;
	int m_nGpuRequiredPages = 0;
	int m_nGpuGuardPages = 0;
	int m_nGpuPrefetchPages = 0;
	int m_nGpuHysteresisPages = 0;
	int m_nGpuPreparedQueue = 0;
	int m_nGpuPreparePendingPages = 0;
	int m_nGpuPrepareJobActive = 0;
	int m_nGpuCookedHits = 0;
	int m_nGpuCookedMisses = 0;
	int m_nGpuRuntimePrepares = 0;
	int m_nGpuFarDirtyRegions = 0;
	int m_nGpuFarRegionsProcessed = 0;
	uint64_t m_nGpuPrepareLatencyUsec = 0u;
	uint64_t m_nGpuPrepareQueueWaitUsec = 0u;
	uint64_t m_nGpuPrepareFileOpenUsec = 0u;
	uint64_t m_nGpuPrepareFileReadUsec = 0u;
	uint64_t m_nGpuPrepareValidationUsec = 0u;
	uint64_t m_nGpuPrepareMaterializeUsec = 0u;
	uint64_t m_nGpuPrepareWorkerUsec = 0u;
	uint64_t m_nGpuPrepareReadyWaitUsec = 0u;
	uint64_t m_nGpuPreparePublicationUsec = 0u;
	uint64_t m_nGpuPrepareEndToEndUsec = 0u;
	uint64_t m_nGpuCacheServiceUsec = 0u;
	int m_nClipmapNearUpdates = 0;
	int m_nClipmapMiddleUpdates = 0;
	int m_nClipmapFarUpdates = 0;
	int m_nClipmapTeleports = 0;
	int m_nClipmapBufferUploads = 0;
	int m_nCpuResident = 0;
	int m_nCpuLoads = 0;
	int m_nCpuEvictions = 0;
	uint64_t m_nCpuLoadBytes = 0u;
	uint64_t m_nCpuCacheServiceUsec = 0u;
	int m_nCpuDesired = 0;
	int m_nCpuSelectionPending = 0;
	int m_nCpuPinned = 0;
	int m_nInstanceResident = 0;
	int m_nInstanceLoads = 0;
	int m_nInstanceEvictions = 0;
	uint64_t m_nInstanceLoadBytes = 0u;
	uint64_t m_nInstanceCacheServiceUsec = 0u;
	int m_nInstanceDesired = 0;
	int m_nInstanceSelectionPending = 0;
	int m_nRenderCells = 0;
	int m_nPendingCellUpdates = 0;
	int m_nNodeActiveRegions = 0;
	int m_nNodeActiveNodes = 0;
	int m_nNodeLoads = 0;
	int m_nNodeUnloads = 0;
	int m_nNodeLoadFailures = 0;
	uint64_t m_nLightmapStreamingUsec = 0u;
} t3dRuntimeProfileSample_t;

// From src/streaming/policy/terrain_3d_streaming_policy.h
typedef struct t3dStreamingCoord_s
{
	int m_nX = 0;
	int m_nY = 0;
} t3dStreamingCoord_t;

// From src/streaming/policy/terrain_3d_streaming_policy.h
typedef struct t3dRegionStreamingConfig_s
{
	int m_nActiveRadius = 1;
	int m_nHotRadius = 8;
	int m_nColdRadius = 12;
	int m_nUnloadGuard = 2;
	bool m_bPredictionEnabled = true;
	double m_fPredictionLookaheadSeconds = 0.75;
	int m_nPredictionMaximumRegions = 4;
} t3dRegionStreamingConfig_t;

// From src/streaming/policy/terrain_3d_streaming_policy.h
typedef struct t3dRuntimeStreamingConfig_s
{
	int m_nActiveRadius = 1;
	int m_nHotRadius = 8;
	int m_nColdRadius = 12;
	int m_nUnloadRadius = 14;
	int m_nUnloadGuard = 2;
	int m_nPredictionMaximumRegions = 4;
	double m_fPredictionLookaheadSeconds = 0.75;
	bool m_bPredictionEnabled = true;
} t3dRuntimeStreamingConfig_t;

// From src/terrain_3d.h
typedef struct t3dBakeRowTask_s
{
	int32_t m_nXStart = 0;
	int32_t m_nXEnd = 0;
	int32_t m_nZStart = 0;
	int32_t m_nZEnd = 0;
	size_t m_iOutputOffset = 0u;
	size_t m_nOutputCapacity = 0u;
} t3dBakeRowTask_t;

// From src/authoring/vt/io/terrain_3d_vt_godot_cooker.cpp
typedef struct t3dVTEditorTimedEncoderV3_s {
	t3dVTEncoderInterface_t m_Base;
	uint64_t m_nEncodeUsec;
} t3dVTEditorTimedEncoderV3_t;

// From src/authoring/vt/build/terrain_3d_vt_page_builder.h
typedef struct t3dVTMipChainLayout_s {
	t3dVTMipLevelLayout_t m_Levels[T3D_VT_MAX_MIPS];
	uint32_t m_nMipCount;
	uint32_t m_nComponents;
	uint64_t m_nTotalBytes;
} t3dVTMipChainLayout_t;

// From src/authoring/world/conversion/terrain_3d_world_godot_converter.h
typedef struct t3dWorldSceneBuildBuffers_s {
	t3dWorldGeometryDatabase_s *pGeometry;
	t3dRenderingMaterialGpu_t *pMaterials;
	uint32_t nMaterialCapacity;
	uint32_t nMaterialCount;
	t3dWorldCell_t *pCells;
	uint32_t nCellCapacity;
	uint32_t nCellCount;
	t3dWorldInstance_t *pInstances;
	t3dWorldTransform_t *pTransforms;
	uint32_t nInstanceCapacity;
	uint32_t nInstanceCount;
	uint64_t *pInstanceSourceIds;
	t3dWorldOwnershipRegistry_t *pOwnership;
	t3dWorldPrototypeEntry_t *pPrototypeEntries;
	t3dWorldPrototypeBounds_t *pPrototypeBoundsByMeshId;
	uint32_t nPrototypeBoundsCapacity;
	uint32_t nPrototypeCapacity;
	uint32_t nPrototypeCount;
	t3dWorldMaterialEntry_t *pMaterialEntries;
	uint32_t nMaterialEntryCapacity;
	uint32_t nMaterialEntryCount;
	t3dWorldNativeVisualEntry_t *pNativeVisuals;
	uint32_t nNativeVisualCapacity;
	uint32_t nNativeVisualCount;
	t3dWorldGodotScratch_t Scratch;
	t3dWorldCellBuildScratch_t CellScratch;
} t3dWorldSceneBuildBuffers_t;

typedef bool ( *t3dWorldPublishConvertedSceneColdFn_t )(
        void *pUser, const t3dWorldSceneBuildBuffers_s &rBuffers, const t3dWorldSceneBuildStats_s &rStats );

// From src/authoring/world/core/terrain_3d_world_controller.h
typedef struct t3dWorldAuthoringBackend_s {
	t3dWorldPublishConvertedSceneColdFn_t pPublishSceneCold = NULL;
	t3dWorldReleaseConvertedSceneColdFn_t pReleaseSceneCold = NULL;
	void *pUser = NULL;
	// CPU acceptance is not a GPU-ready acknowledgement. Required for handoff.
	t3dWorldIsConvertedSceneReadyColdFn_t pIsSceneReadyCold = NULL;
} t3dWorldAuthoringBackend_t;

// From src/data/runtime/terrain_3d_runtime_page_format.h
typedef struct t3dRuntimePageHeader_s {
	uint32_t m_nMagic = T3D_RUNTIME_PAGE_MAGIC;
	uint32_t m_nVersion = T3D_RUNTIME_PAGE_VERSION;
	uint32_t m_nHeaderBytes = T3D_RUNTIME_PAGE_HEADER_BYTES;
	uint32_t m_nReservedGeometry = 0u;
	uint32_t m_nColorDivisor = 1u;
	uint32_t m_nDerivedDivisor = 1u;
	uint32_t m_nChannelFlags = 0u;
	uint32_t m_nTotalBytes = T3D_RUNTIME_PAGE_HEADER_BYTES;
	uint32_t m_nChannelCount = 0u;
	uint32_t m_iChannelTableOffset = 40u;
	t3dRuntimePageChannelInfo_t m_szChannels[T3D_RUNTIME_PAGE_CHANNEL_COUNT];
	uint32_t m_nSourceSignatureLo = 0u;
	uint32_t m_nSourceSignatureHi = 0u;
	uint32_t m_nBlendRecipeVersion = 0u;
	uint32_t m_nMaterialMask = 1u;
	uint32_t m_nMiddleHeightDivisor = 1u;
	uint32_t m_nWorldSignatureLo = 0u;
	uint32_t m_nWorldSignatureHi = 0u;
	int32_t m_nRegionX = 0;
	int32_t m_nRegionZ = 0;
	uint32_t m_nBoundsMinYBits = 0u;
	uint32_t m_nBoundsMaxYBits = 0u;
} t3dRuntimePageHeader_t;

// From src/hlod/runtime/terrain_3d_hlod_streamer.cpp
typedef struct t3dIndexedNodePolicy_s {
	int m_nRegion;
	t3dNodePolicy_t m_policy;
} t3dIndexedNodePolicy_t;

// From src/memory/policy/terrain_3d_memory_budget.h
typedef struct t3dMemoryBudgetInput_s {
	uint64_t m_nTotalBudgetBytes = 0u;
	uint64_t m_nFixedBytes = 0u;
	uint64_t m_nAllocatorContainedFixedBytes = 0u;
	uint64_t m_nAllocatorReserveBytes = 0u;
	uint64_t m_nTransientReserveBytes = 0u;
	t3dMemorySubsystemInput_t m_szSubsystems[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT]{};
} t3dMemoryBudgetInput_t;

// From src/memory/policy/terrain_3d_memory_budget.h
typedef struct t3dMemoryBudgetResult_s {
	uint64_t m_nTotalBudgetBytes = 0u;
	uint64_t m_nFixedBytes = 0u;
	uint64_t m_nAllocatorContainedFixedBytes = 0u;
	uint64_t m_nAllocatorReserveBytes = 0u;
	uint64_t m_nTransientReserveBytes = 0u;
	uint64_t m_nManagedBudgetBytes = 0u;
	uint64_t m_nManualReservedBytes = 0u;
	uint64_t m_nAutoMinimumBytes = 0u;
	uint64_t m_nManagedEffectiveBytes = 0u;
	uint64_t m_nCommittedBytes = 0u;
	uint64_t m_nHeadroomBytes = 0u;
	bool m_bHardOvercommit = false;
	t3dMemorySubsystemAllocation_t m_szSubsystems[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT]{};
} t3dMemoryBudgetResult_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
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
	t3dRdPatch_t m_szPatches[T3D_MAX_CLIPMAP_LEVELS]{};
} t3dRdOceanState_t;

// From src/rendering/rd/core/terrain_3d_rd_types.h
typedef struct alignas( 16 ) t3dRdMaterialConfig_s {
	uint32_t m_nFeatureFlags = 0u;
	uint32_t m_nDebugFlags = 0u;
	uint32_t m_nOutputFlags = T3D_RD_MATERIAL_OUTPUT_ALBEDO | T3D_RD_MATERIAL_OUTPUT_ROUGHNESS | T3D_RD_MATERIAL_OUTPUT_SPECULAR
	        | T3D_RD_MATERIAL_OUTPUT_NORMAL | T3D_RD_MATERIAL_OUTPUT_AO;
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
	float m_fMipmapBias = 0.0f;
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

// From src/rendering/rd/core/terrain_3d_rd_types.h
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

// From src/rendering/rd/instances/terrain_3d_rd_culling.h
typedef struct t3dRdCpuCullResult_s {
	uint32_t m_szVisibleIndices[T3D_RD_DRAW_GROUP_COUNT * T3D_MAX_CLIPMAP_LEVELS];
	t3dRdIndexedIndirectCommand_t m_szCommands[T3D_RD_DRAW_GROUP_COUNT];
	uint32_t m_nVisibleCount;
	uint32_t m_nInvalidPatchCount;
} t3dRdCpuCullResult_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer_contracts.h
// Fixed CPU-only scratch records for bounded visible-material prediction.
typedef struct t3dRdInstanceVtCellPrediction_s {
	uint32_t m_nCell = 0u;
	float m_fDistanceSq = 0.0f;
} t3dRdInstanceVtCellPrediction_t;

typedef struct t3dRdInstanceVtMaterialPrediction_s {
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nCategory = 0u;
	float m_szUvMin[2]{};
	float m_szUvMax[2]{};
	float m_szUvPerPixel[2]{};
	float m_fDistanceSq = 0.0f;
} t3dRdInstanceVtMaterialPrediction_t;

typedef struct t3dRdInstanceDrawPacket_s {
	uint32_t m_nSurfaceSlot = UINT32_MAX;
	uint32_t m_nDrawSlot = UINT32_MAX;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nVisibleCount = 0u;
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nFlags = 0u;
	float m_fSortDistanceSq = 0.0f;
	AABB m_cullBounds;
	uint32_t m_nVisibilityLayers = 1u;
	float m_fLodBegin = 0.0f;
	float m_fLodEnd = 0.0f;
	float m_fFadeMargin = 0.0f;
	float m_fReserved0 = 0.0f;
	t3dRdIndexedIndirectCommand_t m_command{};
} t3dRdInstanceDrawPacket_t;

// From src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h
typedef struct t3dTerrainMegaVTIncrementalCookRequest_s
{
	t3dTerrainMegaVTCookRequest_t m_base;
	uint32_t m_nDirtyMip0Mask;
	uint32_t m_nDirtyPageMask;
	uint32_t m_nSourceGeneration;
	uint32_t m_nIncrementalFlags;
} t3dTerrainMegaVTIncrementalCookRequest_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTCookedSparsePackageV3_s {
	t3dVTCookAllocator_t m_Allocator;
	t3dVTPackAllocationRecordV3_t *m_pAllocations;
	t3dVTPackAllocationKeyRecordV3_t *m_pAllocationKeys;
	t3dVTChannelAllocation_t *m_pChannelAllocations;
	t3dVTPackPageRecordV3_t *m_pPages;
	t3dVTPackPayloadRecordV3_t *m_pPayloads;
	t3dVTPackClusterRecordV3_t *m_pClusters;
	t3dVTPackTailRecordV3_t *m_pTails;
	t3dVTPackQualityViewV3_t *m_pQualityViews;
	uint8_t *m_pPayloadData;
	t3dVTPackFormatFamilyRecordV3_t m_FormatFamilies[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nAllocationCount;
	uint32_t m_nChannelAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nPayloadCount;
	uint32_t m_nClusterCount;
	uint32_t m_nTailCount;
	uint32_t m_nQualityViewCount;
	uint32_t m_nFullySupportedProfileMask;
	uint32_t m_nPackageGeneration;
	uint64_t m_nPayloadDataBytes;
	t3dVTCookSparseStatsV3_t m_Stats;
} t3dVTCookedSparsePackageV3_t;

// From src/rendering/rd/vt/io/terrain_3d_vt_pack_runtime.h
typedef struct t3dVTPackRuntimeStorage_s {
	void *pOwnedBlock;
	uint64_t nOwnedBytes;
	uint8_t *pPackageBytes;
	uint64_t nPackageBytes;
	t3dVTPackViewV3_t PackViewV3;
	t3dVTDatabaseView_t Database;
	t3dVTFileHeader_t *pRuntimeHeader;
	t3dVTAllocationRecordV1_t *pLegacyAllocations;
	t3dVTPageFileRecord_t *pLegacyPages;
	t3dVTPayloadRecord_t *pLegacyPayloads;
	t3dVTAllocation_t *pRuntimeAllocations;
	t3dVTRuntimePayloadRecord_t *pRuntimePayloads;
	t3dVTTailMetadata_t *pRuntimeTails;
	t3dVTRuntimeTailSourceRecord_t *pRuntimeTailSources;
} t3dVTPackRuntimeStorage_t;

// From src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h
typedef struct t3dVTResidencyState_s {
	const t3dVTDatabaseView_t *pDatabase;
	t3dVTResidencyBuffers_t Buffers;
	t3dVTResidencyStats_t Stats;
	t3dVTResidencyPolicy_t Policy;
	uint32_t nNeutralRootPage[T3D_VT_CHANNEL_COUNT];
	uint32_t nClockHand;
	uint32_t nRetireHead;
	uint32_t nRetireTail;
	uint32_t nRetirementDelayFrames;
	uint32_t nDatabaseGeneration;
	uint32_t nPageTableGeneration;
	uint32_t nAllocationCount;
	uint32_t nTailCount;
	bool bInitialized;
	bool bNeutralPublished;
} t3dVTResidencyState_t;

// From src/rendering/rd/world/geometry/terrain_3d_world_geometry.cpp
typedef struct t3dWorldGeometryClusterBuilder_s {
	uint32_t aSourceVertex[T3D_WORLD_CLUSTER_MAX_VERTICES];
	t3dWorldPackedClusterTriangle_t aTriangles[T3D_WORLD_CLUSTER_MAX_TRIANGLES];
	uint32_t nVertexCount;
	uint32_t nTriangleCount;
	uint32_t nMaterialId;
	uint32_t nTier;
} t3dWorldGeometryClusterBuilder_t;

// From src/rendering/rd/ocean/terrain_3d_rd_ocean_renderer.h
typedef struct t3dRdOceanStripSnapshot_s {
	uint64_t m_nGeneration = 0u;
	t3dClipmapStripPart_t m_szStripParts[MESH_TYPE_COUNT][T3D_CLIPMAP_STRIP_PART_CAPACITY]{};
	uint16_t m_szStripPartCounts[MESH_TYPE_COUNT]{};
} t3dRdOceanStripSnapshot_t;

// From src/streaming/policy/terrain_3d_adaptive_budget_policy.h
typedef struct t3dAdaptiveBudgetLimits_s {
	t3dAdaptiveBudgetValues_t m_minimum;
	t3dAdaptiveBudgetValues_t m_maximum;
	uint64_t m_nTargetUsec = 1000u;
	uint64_t m_nHardMaxUsec = 2000u;
	int m_nAdjustmentWindowFrames = 4;
} t3dAdaptiveBudgetLimits_t;

// From src/streaming/policy/terrain_3d_adaptive_budget_policy.h
typedef struct t3dAdaptiveBudgetState_s {
	t3dAdaptiveBudgetValues_t m_effective;
	int m_nOverBudgetStreak = 0;
	int m_nUnderBudgetStreak = 0;
	uint64_t m_nAdjustmentCount = 0u;
	t3dAdaptiveBudgetReason_t m_reason = TERRAIN3D_ADAPTIVE_REASON_DISABLED;
	bool m_bInitialized = false;
} t3dAdaptiveBudgetState_t;

// From src/streaming/policy/terrain_3d_streaming_policy.h
typedef struct t3dRegionStreamingPrediction_s
{
	double m_fVelocityXRegionsPerSecond = 0.0;
	double m_fVelocityYRegionsPerSecond = 0.0;
	t3dStreamingCoord_t m_predictedCenter;
} t3dRegionStreamingPrediction_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTCookPreparedChannelV3_s {
	const uint8_t *m_pMipBytes;
	t3dVTMipChainLayout_t m_Layout;
	uint32_t m_nTargetExtent;
	uint32_t m_nReserved0;
} t3dVTCookPreparedChannelV3_t;

// From src/authoring/vt/io/terrain_3d_vt_godot_cooker.cpp
typedef struct t3dVTEditorChannelCacheDiskHeaderV3_s {
	uint32_t m_nMagic;
	uint32_t m_nVersion;
	uint32_t m_nLogicalChannel;
	uint32_t m_nTargetExtent;
	uint64_t m_nKey;
	t3dVTMipChainLayout_t m_Layout;
	uint64_t m_nPayloadBytes;
	uint32_t m_nPayloadChecksum;
	uint32_t m_nReserved0;
} t3dVTEditorChannelCacheDiskHeaderV3_t;

// From src/rendering/rd/instances/terrain_3d_rd_instance_renderer_contracts.h
typedef struct t3dRdPacketBuildSlot_s {
	t3dRdInstanceDrawPacket_t m_packet;
	uint32_t m_nVisibleCount = 0u;
	uint32_t m_nWriteOffset = 0u;
	uint32_t m_nCommandIndex = UINT32_MAX;
	bool m_bOneSubmitCompatible = false;
} t3dRdPacketBuildSlot_t;

// From src/rendering/rd/core/terrain_3d_render_publication_builder.h
typedef struct t3dRdOceanStripBank_s {
	uint64_t m_nGeneration = 0u;
	t3dRdOceanStripSnapshot_t m_snapshot;
} t3dRdOceanStripBank_t;

// From src/authoring/vt/build/terrain_3d_vt_cooker.h
typedef struct t3dVTCookSparseSourceV3_s {
	uint64_t m_nStableKey;
	t3dVTCookImageView_t m_Channels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint64_t m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_COUNT];
	t3dVTCookPreparedChannelV3_t m_PreparedChannels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nAuthoredChannelMask;
	uint32_t m_nOrmSourceLayout;
	uint32_t m_nWrapMode;
	uint32_t m_nConsumer;
	uint16_t m_nAllocationFlags;
	uint8_t m_bAlphaScissor;
	uint8_t m_nAlphaThreshold;
	uint32_t m_nGenerationSeed;
} t3dVTCookSparseSourceV3_t;

#endif // TERRAIN3D_TYPES_H
