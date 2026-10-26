// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CONSTANTS_H
#define TERRAIN3D_CONSTANTS_H

// Low-level constants required by the EASTL allocator dependency chain.
// POD type-layout constants required while Terrain3DTypes.h is bootstrapped.
#define T3D_CLIPMAP_STRIP_PART_CAPACITY				   ( 24u )
#define T3D_VT_CHANNEL_COUNT						   ( 5u )
#define T3D_VT_LOGICAL_CHANNEL_COUNT				   ( 6u )
#define T3D_VT_QUALITY_PROFILE_COUNT				   ( 6u )
#define T3D_VT_INVALID_ALLOCATION					   ( 0xffffffffu )
#define T3D_VT_MAX_MIPS								   ( 16u )
// Retired numeric spans are retained so downstream shader IDs never shift.
#define T3D_RD_RETIRED_MATERIAL_ID_SLOTS ( 42u )
#define T3D_RD_RETIRED_ALPHA_ID_SLOTS ( 4u )
#define T3D_RD_VT_COLOR_HINT_ID_STRIDE ( 3u )
#define T3D_RD_VT_FEEDBACK_HINT_VARIANT_COUNT ( 2u )

// Native feature gates are explicit build toggles; shader variants are selected
// separately by RDShaderFile. Both default to the established off path.
#ifndef T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS
#define T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS ( 0 )
#endif
#ifndef T3D_ENABLE_COLD_MATERIAL_CLASSES
#define T3D_ENABLE_COLD_MATERIAL_CLASSES ( 0 )
#endif

#ifndef T3D_DEFAULT_TERRAIN_GPU_TIMESTAMPS
#define T3D_DEFAULT_TERRAIN_GPU_TIMESTAMPS ( 0u )
#endif
#define T3D_RD_INSTANCE_DRAW_META_INVALID_MATERIAL	   ( 0x0000FFFFu )
#define T3D_RD_INSTANCE_DRAW_META_DYNAMIC_VISIBLE_BASE ( 0xFFFFFFFFu )

#define T3D_RUNTIME_ALLOCATION_PHASE_COUNT ( 6u )
#define T3D_ROUTE_GATE_CLOSED			   ( UINT32_C( 0x80000000 ) )
#define T3D_ROUTE_READER_MASK			   ( ~T3D_ROUTE_GATE_CLOSED )

#include "core/config/terrain_3d_compiler.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
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

// GDExtension uses the godot namespace, custom modules do not.
#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Centralized project constants. Kept as #define values for the target-era style.
// Static primitive constants centralized from subsystem headers/sources.
// Keep these as macros so they introduce no namespace/static-duration storage.
#define T3D_RD_DESCRIPTOR_BIT( binding ) ( 1ULL << ( binding ) )
#define T3D_RD_DESCRIPTOR_RANGE_MASK( first, last )                                                                    \
	( ( ( 1ULL << ( ( last ) - ( first ) + 1u ) ) - 1ULL ) << ( first ) )
#if INTPTR_MAX == INT64_MAX
#define T3D_EASTL_DEFAULT_ALIGNMENT ( 16u )
#else
#define T3D_EASTL_DEFAULT_ALIGNMENT ( 8u )
#endif

// Formerly local to authoring/world/core/terrain_3d_world_authoring.h.
#define T3D_WORLD_AUTHORING_INVALID_SLOT ( 0xffffffffu )

// Formerly local to authoring/world/core/terrain_3d_world_controller.h.
#define T3D_WORLD_SCRATCH_VERTEX_CAPACITY	( 262144u )
#define T3D_WORLD_SCRATCH_INDEX_CAPACITY	( 786432u )
#define T3D_WORLD_SCRATCH_TRIANGLE_CAPACITY ( 262144u )
#define T3D_WORLD_CELL_HASH_CAPACITY		( 2048u )

// Formerly local to authoring/world/conversion/terrain_3d_world_godot_converter.h.
#define T3D_WORLD_GODOT_SOURCE_FLAG_VISIBLE	 ( 1u << 0u )
#define T3D_WORLD_GODOT_SOURCE_FLAG_EXPLICIT ( 1u << 1u )
#define T3D_WORLD_GODOT_SOURCE_FLAG_EXCLUDED ( 1u << 2u )
#define T3D_WORLD_GODOT_SOURCE_FLAG_DYNAMIC	 ( 1u << 3u )

// Formerly local to data/runtime/terrain_3d_runtime_bake_codec.cpp.
#define T3D_RUNTIME_MIN_MAX_LEVEL_COUNT ( 8u )
#define T3D_RUNTIME_MIN_MAX_BLOB_BYTES	( 174836u )

// Formerly local to gen/doc_data.gen.cpp.
#define T3D_DOC_DATA_UNCOMPRESSED_SIZE ( 0 )
#define T3D_DOC_DATA_COMPRESSED_SIZE   ( 8 )

// Formerly local to hlod/build/terrain_3d_hlod_builder.cpp.
#define T3D_HLOD_CHUNK_TRIANGLE_BUDGET	   ( 131072 )
#define T3D_HLOD_PRE_PROXY_TRIANGLE_BUDGET ( 262144 )
#define T3D_HLOD_MAX_INSTANCES_PER_CHUNK   ( 128 )

// Formerly local to rendering/rd/infrastructure/visibility/terrain_3d_rd_hiz_reducer.h.
#define T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT ( 16u )

// Formerly local to rendering/rd/forward/terrain_3d_forward_pass_preparer.cpp.
#define T3D_FORWARD_PASS_MATERIAL_REQUEST_CAP ( 64u )

// Formerly local to rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h.
#define T3D_CLIPMAP_STRIP_COMMAND_CAPACITY		( T3D_RD_DRAW_GROUP_COUNT * T3D_CLIPMAP_STRIP_PART_CAPACITY )
#define T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES ( sizeof( t3dRdIndexedIndirectCommand_t ) * T3D_RD_DRAW_GROUP_COUNT )
#define T3D_CLIPMAP_STRIP_INDIRECT_BYTES		( sizeof( t3dRdDrawIndirectCommand_t ) * T3D_CLIPMAP_STRIP_COMMAND_CAPACITY )
#define T3D_CLIPMAP_INDIRECT_ARENA_BYTES		( T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES + T3D_CLIPMAP_STRIP_INDIRECT_BYTES )

// Formerly local to rendering/rd/core/terrain_3d_rd_context_contract.h.
#define T3D_RD_FIXED_INDIRECT_ARENA_BYTES ( 4096u )

// Formerly local to rendering/rd/preview/terrain_3d_rd_preview_service.h.
#define T3D_PREVIEW_READBACK_SLOT_COUNT	  ( 3u )
#define T3D_PREVIEW_READBACK_TIMEOUT_USEC ( 2000000u )

// Formerly local to rendering/rd/shaders/terrain_3d_rd_shader_catalog.h.
#define T3D_RD_TERRAIN_FORWARD_SOURCE_VARIANT_COUNT                                                                    \
	( T3D_RD_FORWARD_SHADER_VARIANT_MAX + T3D_RD_FORWARD_COLOR_VARIANT_COUNT )

// Formerly local to rendering/rd/shaders/terrain_3d_rd_shader_library.h.
#define T3D_RD_SHADER_SOURCE_CAPACITY ( 25u )

// Formerly local to rendering/rd/core/terrain_3d_rd_types.h.
#define T3D_RD_FORWARD_COLOR_VARIANT_COUNT ( 8u )

// Formerly local to rendering/rd/core/terrain_3d_render_coordinator.cpp.
#define T3D_RENDER_COORDINATOR_WORK_UNIT_BUDGET	  ( 1u )
#define T3D_RENDER_COORDINATOR_UPLOAD_BYTE_BUDGET ( 32u * 1024u * 1024u )

// Formerly local to rendering/rd/core/terrain_3d_render_lifecycle_state.h.
#define T3D_RENDER_READY_STARTUP_REQUIRED                                                                              \
	( T3D_RENDER_READY_SHADER_GPU | T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE |                     \
	  T3D_RENDER_READY_VT_PUBLISHED | T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD | T3D_RENDER_READY_OCEAN )
#define T3D_RENDER_READY_RESOURCE_CONSUMERS                                                                            \
	( T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED |                   \
	  T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD | T3D_RENDER_READY_OCEAN | T3D_RENDER_READY_PREVIEW )

// Formerly local to Terrain3DConstants.h.
#define T3D_RD_TERRAIN_SET0_MASK		 ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 38u ) )
#define T3D_RD_TERRAIN_SET2_MASK		 ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 15u ) )
#define T3D_RD_TERRAIN_PUSH_BYTES		 ( 16u )
#define T3D_RD_TERRAIN_VERTEX_INPUT_MASK ( 0u )
#define T3D_RD_WORLD_TERRAIN_SET0_MASK                                                                                 \
	( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 25u ) | T3D_RD_DESCRIPTOR_BIT( 49u ) | T3D_RD_DESCRIPTOR_BIT( 50u ) )
#define T3D_RD_WORLD_TERRAIN_SET1_MASK ( T3D_RD_DESCRIPTOR_RANGE_MASK( 1u, 12u ) )
#define T3D_RD_WORLD_MESH_SET0_MASK                                                                                    \
	( T3D_RD_DESCRIPTOR_BIT( 0u ) | T3D_RD_DESCRIPTOR_RANGE_MASK( 39u, 42u ) |                                         \
	  T3D_RD_DESCRIPTOR_RANGE_MASK( 46u, 49u ) )
#define T3D_RD_WORLD_MESH_SET1_MASK			  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 12u ) )
#define T3D_RD_WORLD_RASTER_PUSH_BYTES		  ( 0u )
#define T3D_RD_WORLD_RASTER_VERTEX_INPUT_MASK ( 0u )
#define T3D_RD_INSTANCE_SET0_MASK			  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 6u ) )
#define T3D_RD_INSTANCE_SET1_MASK			  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 10u ) )
#define T3D_RD_INSTANCE_PUSH_BYTES			  ( 32u )
#define T3D_RD_INSTANCE_VERTEX_INPUT_MASK	  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 5u ) )
#define T3D_RD_OCEAN_SET0_MASK				  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 2u ) )
#define T3D_RD_OCEAN_VERTEX_INPUT_MASK		  ( 0u )
#define T3D_RD_WORLD_VIS_RESET_SET0_MASK	  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 2u ) )
#define T3D_RD_WORLD_VIS_CULL_SET0_MASK		  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 12u ) )
#define T3D_RD_WORLD_VIS_FINALIZE_SET0_MASK	  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 2u ) )
#define T3D_RD_WORLD_VIS_PUSH_BYTES			  ( 48u )
#define T3D_RD_WORLD_HIZ_SET0_MASK			  ( T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 1u ) )
#define T3D_RD_WORLD_HIZ_PUSH_BYTES			  ( 16u )

// Formerly local to rendering/rd/vt/streaming/terrain_3d_vt_residency.h.
#define T3D_VT_LEGACY_FILE_MAGIC		( 0x56543354u )
#define T3D_VT_LEGACY_FILE_VERSION		( 1u )
#define T3D_VT_PAGE_SET_CHANNEL_MASK	( T3D_VT_CHANNEL_MASK_ALL )
#define T3D_VT_RESIDENCY_CATEGORY_COUNT ( 5u )

// Formerly local to rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h.
#define T3D_VT_GPU_FRAME_RING_COUNT ( 3u )
#define T3D_VT_MAX_TEXTURE_BANKS	( 1u )

// Formerly local to rendering/rd/vt/core/terrain_3d_vt_runtime_contract.h.
#define T3D_VT_INVALID_ID ( 0xffffffffu )

// Formerly local to rendering/rd/vt/gpu/terrain_3d_vt_shared_bindings.h.
#define T3D_SHARED_VT_CHANNEL_COUNT		( T3D_VT_CHANNEL_COUNT )
#define T3D_SHARED_VT_MAX_TEXTURE_BANKS ( 1u )

// Formerly local to rendering/rd/vt/streaming/terrain_3d_vt_streaming.cpp.
#define T3D_VT_INVALID_JOB16 ( 0xffffu )

// Formerly local to rendering/rd/vt/streaming/terrain_3d_vt_streaming.h.
#define T3D_VT_MAX_STREAM_SCRATCH_LANES ( 8u )
#define T3D_VT_PRIORITY_BUCKET_COUNT	( 6u )
#define T3D_VT_PRIORITY_CRITICAL_MIN	( 8192u )
#define T3D_VT_PRIORITY_NEAR_MIN		( 4096u )
#define T3D_VT_PRIORITY_NORMAL_MIN		( 2048u )
#define T3D_VT_PRIORITY_PREFETCH_MIN	( 1024u )
#define T3D_VT_PRIORITY_TRANSITION_MIN	( 512u )

// Formerly local to rendering/rd/world/core/terrain_3d_world_contract.h.
#define T3D_WORLD_ABI_VERSION							 ( 2u )
#define T3D_WORLD_INVALID_ID							 ( 0xffffffffu )
#define T3D_WORLD_CLUSTER_MAX_VERTICES					 ( 64u )
#define T3D_WORLD_CLUSTER_MAX_TRIANGLES					 ( 124u )
#define T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT			 ( 372u )
#define T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT		 ( 372u )
#define T3D_WORLD_TERRAIN_STRIP_MAX_CELL_WIDTH			 ( 184u )
#define T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT			 ( 6u )
#define T3D_WORLD_TERRAIN_STRIP_BUCKET_0_VERTEX_COUNT	 ( 64u )
#define T3D_WORLD_TERRAIN_STRIP_BUCKET_1_VERTEX_COUNT	 ( 128u )
#define T3D_WORLD_TERRAIN_STRIP_BUCKET_2_VERTEX_COUNT	 ( 192u )
#define T3D_WORLD_TERRAIN_STRIP_BUCKET_3_VERTEX_COUNT	 ( 256u )
#define T3D_WORLD_TERRAIN_STRIP_BUCKET_4_VERTEX_COUNT	 ( 320u )
#define T3D_WORLD_TERRAIN_STRIP_BUCKET_5_VERTEX_COUNT	 ( 372u )
#define T3D_WORLD_INDIRECT_TERRAIN_STRIP_INDEX			 ( 0u )
#define T3D_WORLD_INDIRECT_NONTERRAIN_INDEX				 ( T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT )
#define T3D_WORLD_INDIRECT_COMMAND_COUNT				 ( T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT + 1u )
#define T3D_WORLD_FRAME_FLAG_HIZ_VALID					 ( 1u << 0u )
#define T3D_WORLD_VIS_COUNTER_VISIBLE					 ( 0u )
#define T3D_WORLD_VIS_COUNTER_CANDIDATE					 ( 1u )
#define T3D_WORLD_VIS_COUNTER_FRUSTUM_REJECT			 ( 2u )
#define T3D_WORLD_VIS_COUNTER_LOD_REJECT				 ( 3u )
#define T3D_WORLD_VIS_COUNTER_CONE_REJECT				 ( 4u )
#define T3D_WORLD_VIS_COUNTER_HIZ_REJECT				 ( 5u )
#define T3D_WORLD_VIS_COUNTER_OVERFLOW					 ( 6u )
#define T3D_WORLD_VIS_COUNTER_TERRAIN					 ( 7u )
#define T3D_WORLD_VIS_COUNTER_MESH						 ( 8u )
#define T3D_WORLD_VIS_COUNTER_HLOD						 ( 9u )
#define T3D_WORLD_VIS_COUNTER_STATIC					 ( 10u )
#define T3D_WORLD_VIS_COUNTER_NONTERRAIN				 ( 11u )
#define T3D_WORLD_VIS_COUNTER_TERRAIN_BUCKET_BASE		 ( 12u )
#define T3D_WORLD_VIS_COUNTER_TERRAIN_USEFUL_VERTICES	 ( 18u )
#define T3D_WORLD_VIS_COUNTER_TERRAIN_SUBMITTED_VERTICES ( 19u )
#define T3D_WORLD_VIS_COUNTER_COUNT						 ( 20u )
#define T3D_WORLD_VERTEX_SIZE_BYTES						 ( 32u )
#define T3D_WORLD_CLUSTER_SIZE_BYTES					 ( 64u )
#define T3D_WORLD_TERRAIN_CLUSTER_SIZE_BYTES			 ( 32u )
#define T3D_WORLD_TRANSFORM_SIZE_BYTES					 ( 48u )
#define T3D_WORLD_VISIBLE_ITEM_SIZE_BYTES				 ( 16u )
#define T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES			 ( 16u )
#define T3D_WORLD_INDIRECT_BUFFER_SIZE_BYTES                                                                           \
	( T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES * T3D_WORLD_INDIRECT_COMMAND_COUNT )
#define T3D_WORLD_FRAME_SIZE_BYTES				  ( 336u )
#define T3D_WORLD_RESOURCE_LIMITS_SIZE_BYTES	  ( 96u )
#define T3D_WORLD_TERRAIN_PATCH_FLAGS_MASK		  ( 0x0000ffffu )
#define T3D_WORLD_TERRAIN_TIER_SHIFT			  ( 16u )
#define T3D_WORLD_TERRAIN_TIER_MASK				  ( 0x00030000u )
#define T3D_WORLD_TERRAIN_SERPENTINE_ROWS_BIT	  ( 18u )
#define T3D_WORLD_TERRAIN_SERPENTINE_BASE_ODD_BIT ( 19u )
#define T3D_WORLD_TERRAIN_STITCH_X_SHIFT		  ( 20u )
#define T3D_WORLD_TERRAIN_STITCH_X_MASK			  ( 0x00300000u )
#define T3D_WORLD_TERRAIN_STITCH_Z_SHIFT		  ( 22u )
#define T3D_WORLD_TERRAIN_STITCH_Z_MASK			  ( 0x00c00000u )
#define T3D_WORLD_TERRAIN_PART_CODE_SHIFT		  ( 24u )
#define T3D_WORLD_TERRAIN_PART_CODE_MASK		  ( 0x07000000u )

// Formerly local to rendering/rd/world/geometry/terrain_3d_world_geometry.h.
#define T3D_WORLD_GEOMETRY_SIGNATURE_VERSION ( 1u )

// Formerly local to rendering/rd/world/visibility/terrain_3d_world_raster.h.
#define T3D_WORLD_PIPELINE_CAPACITY		  ( 32u )
#define T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT ( T3D_RD_FORWARD_SHADER_VARIANT_MAX + 1u )
#define T3D_WORLD_SHADOW_DESCRIPTOR_SLOT  ( T3D_RD_FORWARD_SHADER_VARIANT_MAX )

// Formerly local to rendering/material/terrain_3d_shader_specializer.h.
#define T3D_TERRAIN_SPEC_CONSTANT_SHIPPING_FEATURE_CLASS  ( 24u )
#define T3D_TERRAIN_SHIPPING_FEATURE_CLASS_COUNT		  ( 8u )
#define T3D_TERRAIN_SPEC_CONSTANT_LIGHTING_MODEL_CLASS	  ( 25u )
#define T3D_TERRAIN_SPEC_CONSTANT_SCENE_INTEGRATION_CLASS ( 26u )
#define T3D_TERRAIN_LIGHTING_MODEL_CLASS_COUNT			  ( 4u )
#define T3D_TERRAIN_SCENE_INTEGRATION_CLASS_COUNT		  ( 2u )
#define T3D_TERRAIN_SPEC_CONSTANT_DNR_OUTPUT_CLASS		  ( 27u )
#define T3D_TERRAIN_SPEC_CONSTANT_MIDDLE_SURFACE_CLASS	  ( 28u )
// noise-cache specialization: bounded cold noise and exclusive editor diagnostic classes.
#define T3D_TERRAIN_SPEC_CONSTANT_EDITOR_DIAGNOSTIC_CLASS ( 29u )
#define T3D_TERRAIN_SPEC_CONSTANT_NOISE_SOURCE_CLASS ( 30u )
#define T3D_TERRAIN_EDITOR_DIAGNOSTIC_RUNTIME ( 0u )
#define T3D_TERRAIN_EDITOR_DIAGNOSTIC_HEIGHT ( 1u )
#define T3D_TERRAIN_EDITOR_DIAGNOSTIC_CONTROL ( 2u )
#define T3D_TERRAIN_EDITOR_DIAGNOSTIC_CLASS_COUNT ( 3u )
#define T3D_TERRAIN_NOISE_SOURCE_RUNTIME ( 0u )
#define T3D_TERRAIN_NOISE_SOURCE_CACHE ( 1u )
#define T3D_TERRAIN_NOISE_SOURCE_CLASS_COUNT ( 2u )
#define T3D_TERRAIN_DNR_OUTPUT_CLASS_COUNT				  ( 4u )
#define T3D_TERRAIN_MIDDLE_SURFACE_CLASS_COUNT			  ( 2u )

// Formerly local to streaming/policy/terrain_3d_performance_profile.h.
#define T3D_PERFORMANCE_PROFILE_SIGNATURE ( t3dPerformanceProfileComputeSignature() )

#define T3D_PROFILE_VERSION				  ( 6u )
#define T3D_PROFILE_FNV_OFFSET			  ( 1469598103934665603ull )
#define T3D_PROFILE_FNV_PRIME			  ( 1099511628211ull )
#define T3D_HASH_GOLDEN_RATIO32			  ( 0x9e3779b9u )
#define T3D_HASH_MIX32					  ( 0x85ebca6bu )
#define T3D_HASH_MIX32_SECONDARY		  ( 0xc2b2ae35u )
#define T3D_HASH_GOLDEN_RATIO64			  ( 0x9e3779b97f4a7c15ULL )
#define s_fUint32MaxFloat				  ( 4294967295.0f )
#define T3D_TEST_HALF_ENCODING_COUNT	  ( 65536u )
#define T3D_TEST_TRANSFORM_COUNT		  ( 37u )
#define T3D_TEST_NORMAL_WIDTH			  ( 37 )
#define T3D_TEST_NORMAL_HEIGHT			  ( 19 )
#define T3D_TEST_NORMAL_STRIDE			  ( T3D_TEST_NORMAL_WIDTH + 2 )
#define T3D_TEST_EROSION_COUNT			  ( 4099u )
#define T3D_TEST_NON_FINITE_EROSION_COUNT ( 16u )
#define T3D_TEST_DISPATCH_COUNT			  ( 1024u )
#define T3D_TEST_DISPATCH_ROUNDS		  ( 32u )
#define T3D_TEST_DISPATCH_ITERATIONS	  ( 64u )
#define T3D_TEST_EASTL_ALIGNMENT		  ( 64u )
#define T3D_TEST_EASTL_OFFSET			  ( 13u )
#define T3D_TEST_CONTROL_FLAG			  ( 1u << 0u )
#define T3D_MEMORY_SUBSYSTEM_NAME_COUNT	  ( sizeof( s_pszMemorySubsystemNames ) / sizeof( s_pszMemorySubsystemNames[0] ) )
#define s_shadowCastingOn				  ( GeometryInstance3D::SHADOW_CASTING_SETTING_ON )
#define s_shadowCastingOff				  ( GeometryInstance3D::SHADOW_CASTING_SETTING_OFF )
#define s_shadowCastingOnly				  ( GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY )
#define T3D_MAX_TEXTURE_ASSETS			  ( 32 )
#define T3D_MAX_MESH_ASSETS				  ( 256 )
#define s_fDataCurrentVersion			  ( 0.95f )
#define s_fRegionHeightMin				  ( -384.0f )
#define s_fRegionHeightMax				  ( 768.0f )
#define s_fRegionHeightRange			  ( s_fRegionHeightMax - s_fRegionHeightMin )
#define s_fRegionHeightInvRange			  ( 1.0f / s_fRegionHeightRange )
#define s_fRegionHeightU16Scale			  ( 65535.0f )
#define T3D_REGION_DEFAULT_CONTROL		  ( 1u )
#define T3D_AUTOSHADER_MATERIAL_MASK	  ( ( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u ) )
#define s_fDegreesPerRadian				  ( 57.29577951308232f )
#define s_fSqrtTwo						  ( 1.4142135623730951f )
#define s_fLargeDistance				  ( 1.0e20f )
#define T3D_LANDFORM_MATERIAL_MASK		  ( T3D_AUTOSHADER_MATERIAL_MASK | 0x1u )
#define s_fLandformEpsilon				  ( 0.0001f )
#define T3D_EROSION_DIRECTION_COUNT		  ( 4 )
static const int32_t g_t3dConstStorage_s_szErosionDirectionX[T3D_EROSION_DIRECTION_COUNT] = { -1, 1, 0, 0 };
#define s_szErosionDirectionX g_t3dConstStorage_s_szErosionDirectionX
static const int32_t g_t3dConstStorage_s_szErosionDirectionY[T3D_EROSION_DIRECTION_COUNT] = { 0, 0, -1, 1 };
#define s_szErosionDirectionY g_t3dConstStorage_s_szErosionDirectionY
static const int32_t g_t3dConstStorage_s_szErosionOppositeDirection[T3D_EROSION_DIRECTION_COUNT] = { 1, 0, 3, 2 };
#define s_szErosionOppositeDirection			   g_t3dConstStorage_s_szErosionOppositeDirection
#define s_fErosionEpsilon						   ( 1.0e-6f )
#define T3D_EROSION_FNV_OFFSET					   ( 1469598103934665603ULL )
#define T3D_EROSION_FNV_PRIME					   ( 1099511628211ULL )
#define T3D_EROSION_HALF_DECODE_BLOCK			   ( 256u )
#define s_fClipmapPartStatic					   ( 0.0f )
#define s_fClipmapPartEdgeA0					   ( 1.0f )
#define s_fClipmapPartEdgeA1					   ( 2.0f )
#define s_fClipmapPartEdgeB0					   ( 3.0f )
#define s_fClipmapPartEdgeB1					   ( 4.0f )
#define T3D_MULTIMESH_STRIDE_FLOATS				   ( 16 )
#define T3D_COMPUTE_WORKGROUP_SIZE				   ( 64u )
#define T3D_SHADER_MAX_CONDITIONAL_DEPTH		   ( 32 )
#define T3D_INSTANCE_PAGE_MAGIC					   ( 0x49334454u )
#define T3D_INSTANCE_PAGE_VERSION				   ( 2u )
#define T3D_INSTANCE_PAGE_LEGACY_VERSION		   ( 1u )
#define T3D_INSTANCE_PAGE_HEADER_BYTES			   ( 16u )
#define T3D_INSTANCE_PAGE_MESH_HEADER_BYTES		   ( 12u )
#define T3D_INSTANCE_PAGE_LEGACY_MESH_HEADER_BYTES ( 8u )
#define T3D_INSTANCE_PAGE_CELL_HEADER_BYTES		   ( 12u )
#define T3D_INSTANCE_PAGE_CELL_COLORS			   ( 1u << 0u )
#define T3D_INSTANCE_PAGE_CELL_OWNERS			   ( 1u << 1u )
#define T3D_INSTANCE_PAGE_CELL_FLAGS			   ( T3D_INSTANCE_PAGE_CELL_COLORS | T3D_INSTANCE_PAGE_CELL_OWNERS )
#define T3D_ACTIVE_ACCESS_READER_MASK			   ( 0x7fffffffu )
#define T3D_ACTIVE_ACCESS_WRITER_BIT			   ( 0x80000000u )
#define T3D_ACTIVE_STORAGE_ALIGNMENT			   ( 64u )
#define T3D_AUTOSHADER_MODE_LEGACY_RUNTIME		   ( 0 )
#define T3D_AUTOSHADER_MODE_BAKED				   ( 1 )
#define T3D_AUTOSHADER_MODE_DISABLED			   ( 2 )
#define T3D_AUTOSHADER_MODE_EDITOR_PREVIEW		   ( 3 )
#define T3D_BAKE_CACHELINE_BYTES				   ( 64u )
#define T3D_COMPUTE_SCRATCH_RETAIN_LIMIT		   ( 32u * 1024u * 1024u )
#define T3D_CPU_CACHE_BYTES_PER_TEXEL			   ( 12u )
#define T3D_CPU_CACHE_MAX_LAYERS				   ( 64 )
#define T3D_CPU_CACHE_MIB						   ( 1024u * 1024u )
#define T3D_CPU_CACHE_MIN_SAMPLE_LAYERS			   ( 4 )
#define T3D_CPU_REGION_CACHE_MAX_BUDGET_MB		   ( 128 )
#define T3D_CPU_REGION_CACHE_MIN_BUDGET_MB		   ( 1 )
#define T3D_CPU_REGION_LOAD_MAX_BUDGET_MB		   ( 64 )
#define T3D_CPU_REGION_LOAD_MAX_LAYERS			   ( 64 )
#define T3D_DEFAULT_RESIDENT_POOL_BUDGET		   ( 64u * 1024u * 1024u )
#define T3D_DIAGNOSTICS_VERSION					   ( 1 )
#define T3D_DIRECTIONAL_RECEIVER_LAYER			   ( 1u << 19u )
#define T3D_EROSION_TEXTURE_PROFILE_COUNT		   ( 32 )
#define T3D_FEATURE_ALT_VALUE					   ( 1u << 0u )
#define T3D_FEATURE_ANGLE_VALUE					   ( 1u << 3u )
#define T3D_FEATURE_CENTER_VALID_VALUE			   ( 1u << 1u )
#define T3D_FEATURE_SCALE_VALUE					   ( 1u << 4u )
#define T3D_FEATURE_TEXTURE_VALUE				   ( 1u << 2u )
#define T3D_GPU_HEIGHT_CACHE_MAX_LAYERS			   ( 512 )
#define T3D_GPU_HEIGHT_CACHE_MIN_SPARE_LAYERS	   ( 8 )
#define T3D_GPU_REGION_CACHE_MAX_BUDGET_MB		   ( 512 )
#define T3D_GPU_REGION_CACHE_MIN_BUDGET_MB		   ( 1 )
#define T3D_GPU_REGION_UPLOAD_MAX_BUDGET_MB		   ( 64 )
#define T3D_GPU_REGION_UPLOAD_MAX_LAYERS		   ( 64 )
#define T3D_INSTANCE_DEFAULT_CELL_SIZE			   ( 32 )
#define T3D_INSTANCE_MAX_CELLS_PER_AXIS			   ( 16 )
#define T3D_INSTANCE_MAX_CELL_SIZE				   ( 128 )
#define T3D_INSTANCE_MAX_REGION_SIZE			   ( T3D_REGION_SIZE )
#define T3D_INSTANCE_MIN_CELL_SIZE				   ( 8 )
#define T3D_INSTANCE_MIN_REGION_SIZE			   ( T3D_REGION_SIZE )
#define T3D_INSTANCE_REGION_CACHE_MAX_BUDGET_MB	   ( 128 )
#define T3D_INSTANCE_REGION_CACHE_MIN_BUDGET_MB	   ( 1 )
#define T3D_INSTANCE_REGION_LOAD_MAX_BUDGET_MB	   ( 64 )
#define T3D_INSTANCE_REGION_LOAD_MAX_PAGES		   ( 64 )
#define T3D_INSTANCE_TRANSITION_BUCKET_CELL_SPAN   ( 4 )
#define T3D_INSTANCE_WHITE_RGBA8				   ( 0xffffffffu )
#define T3D_LIGHTING_LAYER_FOLIAGE				   ( 1u << 15u )
#define T3D_LIGHTING_LAYER_LARGE				   ( 1u << 17u )
#define T3D_LIGHTING_LAYER_MEDIUM				   ( 1u << 16u )
#define T3D_LIGHTING_LAYER_REGIONAL				   ( 1u << 18u )
#define T3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT		   ( 8 )
#define T3D_LIGHTMAP_FNV_OFFSET					   ( 1469598103934665603ull )
#define T3D_LIGHTMAP_FNV_PRIME					   ( 1099511628211ull )
#define T3D_MAX_CLIPMAP_LEVELS					   ( 16 )
#define T3D_MAX_REGION_BLEND_SAMPLES			   ( T3D_REGION_SIZE )
#define T3D_MAX_COLLISION_AXIS_SAMPLES			   ( T3D_MAX_REGION_BLEND_SAMPLES + 1 )
#define T3D_MAX_COLLISION_SHAPE_RECORDS			   ( 4096 )
#define T3D_MAX_RENDER_BATCHES					   ( 5 )
#define T3D_MAX_SHADER_VARIANT_CACHE_LIMIT		   ( 256 )
#define T3D_MAX_SHADER_VARIANT_PREWARM_LIMIT	   ( 64 )
#define T3D_MAX_SHADER_VARIANT_SOURCE_BUDGET_MB	   ( 256 )
#define T3D_MAX_TILE_INSTANCES					   ( 16 )
#define T3D_MEMORY_MIB							   ( 1024ULL * 1024ULL )
#define T3D_MESH_MAX_LOD_COUNT					   ( 10 )
#define T3D_MESH_SHADOW_LOD_ID					   ( -1 )
#define T3D_MIN_SHADER_VARIANT_CACHE_LIMIT		   ( 8 )
#define T3D_MIN_SHADER_VARIANT_PREWARM_LIMIT	   ( 1 )
#define T3D_MIN_SHADER_VARIANT_SOURCE_BUDGET_MB	   ( 1 )
#define T3D_OP_ADD_VALUE						   ( 0u )
#define T3D_OP_GRADIENT_VALUE					   ( 4u )
#define T3D_OP_REPLACE_VALUE					   ( 2u )
#define T3D_OP_SUBTRACT_VALUE					   ( 1u )
#define T3D_PAIR_INSTANCES						   ( 2 )
#define T3D_RENDER_CONTROL_BLEND_RADIUS			   ( 2 )
static const int g_t3dConstStorage_s_szRenderControlKernel[5] = { 1, 2, 3, 2, 1 };
#define s_szRenderControlKernel				 g_t3dConstStorage_s_szRenderControlKernel
#define T3D_RENDER_CONTROL_MATERIAL_MASK	 ( ( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u ) )
#define T3D_RESIDENT_CACHE_ALIGNMENT		 ( 64u )
#define T3D_RESIDENT_INVALID_OFFSET			 ( ( size_t ) ( -1 ) )
#define T3D_RING_TILE_INSTANCES				 ( 12 )
#define T3D_RD_HOT_RECORD_CAPACITY			 ( 256u )
#define T3D_RUNTIME_PROFILE_HISTORY_CAPACITY ( 120u )
#define T3D_SAMPLER_CACHE_SIZE				 ( 8 )
#define T3D_TOOL_AUTOSHADER_VALUE			 ( 6u )
#define T3D_TOOL_COLOR_VALUE				 ( 4u )
#define T3D_TOOL_HEIGHT_VALUE				 ( 2u )
#define T3D_TOOL_HOLES_VALUE				 ( 7u )
#define T3D_TOOL_NAVIGATION_VALUE			 ( 8u )
#define T3D_TOOL_ROUGHNESS_VALUE			 ( 5u )
#define T3D_TOOL_TEXTURE_VALUE				 ( 3u )
#define T3D_TYPE_COLOR_VALUE				 ( 2u )
#define T3D_TYPE_CONTROL_VALUE				 ( 1u )
#define T3D_TYPE_HEIGHT_VALUE				 ( 0u )
#define T3D_TYPE_MAX_VALUE					 ( 3u )
#define T3D_WORLD_TEXELS					 ( 4096 )
#define T3D_REGION_SIZE						 ( 128 )
#define T3D_REGION_VERTEX_SPACING_METERS	 ( 1u )
#define s_fRegionVertexSpacing				 ( 1.0f )
#define T3D_REGION_WORLD_SIZE_METERS		 ( T3D_REGION_SIZE * T3D_REGION_VERTEX_SPACING_METERS )
#define T3D_REGION_SHIFT					 ( 7 )
#define T3D_REGION_MASK						 ( 127 )
#define T3D_REGION_MAP_SIZE					 ( 32 )
#define T3D_REGION_MAP_SHIFT				 ( 5 )
#define T3D_REGION_MAP_MASK					 ( 31 )
#define T3D_REGION_SLOTS					 ( 1024 )
#define T3D_REGION_MAP_HALF					 ( 16 )
#define T3D_REGION_TEXELS					 ( 16384 )
#define T3D_REGION_HEIGHT_BYTES				 ( 32768 )
#define T3D_REGION_GEOMETRY_SIGNATURE                                                                                  \
	( 0x80000000u | ( ( uint32_t ) ( T3D_REGION_SIZE ) << 16u ) |                                                      \
	  ( ( uint32_t ) ( T3D_REGION_VERTEX_SPACING_METERS ) << 8u ) | ( uint32_t ) ( T3D_REGION_MAP_SIZE ) )
#define T3D_XBOX360_CPU_MAP_CEILING_BYTES  ( 64u * 1024u * 1024u )
#define T3D_ACTIVE_WORLD_TEXELS			   ( T3D_WORLD_TEXELS )
#define T3D_BAKE_UV_CACHELINE_ELEMENTS	   ( T3D_BAKE_CACHELINE_BYTES / sizeof( Vector2 ) )
#define T3D_BAKE_VERTEX_CACHELINE_ELEMENTS ( T3D_BAKE_CACHELINE_BYTES / ( sizeof( Vector3 ) / 3u ) )
#define T3D_LIGHTING_CLASS_LAYER_MASK                                                                                  \
	( T3D_LIGHTING_LAYER_FOLIAGE | T3D_LIGHTING_LAYER_MEDIUM | T3D_LIGHTING_LAYER_LARGE | T3D_LIGHTING_LAYER_REGIONAL )
#define T3D_MAX_BATCH_INSTANCES					  ( T3D_MAX_CLIPMAP_LEVELS )
#define T3D_MAX_BUFFER_SHADER_VARIANT_CACHE_LIMIT ( T3D_MAX_SHADER_VARIANT_CACHE_LIMIT / 2 )
#define T3D_MAX_EDITOR_SHADER_VARIANT_CACHE_LIMIT ( T3D_MAX_SHADER_VARIANT_CACHE_LIMIT / 4 )
#define T3D_MEMORY_POLICY_HYSTERESIS_BYTES		  ( 2ULL * T3D_MEMORY_MIB )
#define T3D_BAKE_TASK_ALIGNMENT_ELEMENTS		  ( T3D_BAKE_VERTEX_CACHELINE_ELEMENTS )
#define T3D_LIGHTING_MANAGED_LAYER_MASK			  ( T3D_LIGHTING_CLASS_LAYER_MASK | T3D_DIRECTIONAL_RECEIVER_LAYER )
#define T3D_RESIDENT_WORLD_SIZE					  ( T3D_REGION_MAP_SIZE )
#define T3D_INSTANCE_REGION_TABLE_CAPACITY		  ( ( size_t ) ( T3D_REGION_SLOTS ) )
#define T3D_RESIDENT_WORLD_HALF					  ( T3D_RESIDENT_WORLD_SIZE / 2 )
#define T3D_RESIDENT_WORLD_SLOT_COUNT			  ( T3D_REGION_SLOTS )
#define T3D_INSTANCE_REGION_UPDATE_CAPACITY		  ( T3D_INSTANCE_REGION_TABLE_CAPACITY )
#define s_fControlAutoFloat						  ( FLT_TRUE_MIN )
#define MAX_TEXTURE_ASSETS						  ( T3D_MAX_TEXTURE_ASSETS )
#define MAX_MESH_ASSETS							  ( T3D_MAX_MESH_ASSETS )
#define T3D_COOK_DATABASE_MAGIC					  ( 0x3142444b43443354ull ) // T3DCKDB1
#define T3D_COOK_DATABASE_VERSION				  ( 2u )
#define T3D_COOK_DATABASE_HEADER_BYTES			  ( 40u )
#define s_fByteMaxFloat							  ( 255.0f )
#define s_fByteMidpointFloat					  ( 127.5f )
#define s_fByteInvMaxFloat						  ( 1.0f / s_fByteMaxFloat )
#define T3D_UNIT_TESTING_PUBLICATION_COUNT		  ( 10000u )
#define T3D_UNIT_TESTING_CHECK_MASK				  ( 0x9e3779b97f4a7c15ULL )
#define T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES	  ( sizeof( t3dRdIndexedIndirectCommand_t ) * T3D_RD_DRAW_GROUP_COUNT )
#define T3D_COMPUTE_BATCH_SIZE					  ( 3u )
#define T3D_HOLE_MAGIC							  ( 0x314c4f48u ) // HOL1
#define T3D_NAV_MAGIC							  ( 0x3156414eu ) // NAV1
#define T3D_MINMAX_MAGIC						  ( 0x31584d4du ) // MMX1
#define T3D_COLLISION_HEIGHT_MAGIC				  ( 0x31484c43u ) // CLH1
#define T3D_COLLISION_BOUNDS_MAGIC				  ( 0x31424c43u ) // CLB1
#define T3D_NODE_MAGIC							  ( 0x31444f4eu ) // NOD1
#define T3D_INSTANCE_META_MAGIC					  ( 0x314d4e49u ) // INM1
#define T3D_LIGHTING_MAGIC						  ( 0x3154474cu ) // LGT1
#define T3D_MULTIMESH_MAGIC						  ( 0x31494d4du ) // MMI1
#define T3D_BLOB_VERSION						  ( 2u )
#define T3D_FIXED								  ( 8u + 2u + 2u + 4u + 2u + 2u + 16u + 2u + 2u + 4u + 48u + 4u )
#define T3D_RUNTIME_PAGE_ALIGNMENT				  ( 16u )
#define T3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION	  ( 4u )
#define T3D_RUNTIME_PAGE_CHANNEL_COUNT			  ( 24u )
#define T3D_RUNTIME_PAGE_HEADER_BYTES			  ( 1248u )
#define T3D_RUNTIME_PAGE_MAGIC					  ( 0x47503354u )
#define T3D_RUNTIME_PAGE_MAX_BYTES				  ( 256u * 1024u * 1024u )
#define T3D_RUNTIME_PAGE_VERSION				  ( 7u )
#define T3D_MANIFEST_HEADER_BYTES				  ( 112u )
#define T3D_MANIFEST_TOPOLOGY_BYTES				  ( T3D_REGION_SLOTS / 8u )
#define T3D_MANIFEST_REGION_BASE_BYTES			  ( 112u )
#define T3D_RUNTIME_WORLD_MANIFEST_MAGIC		  ( 0x31464e4d57334454ull ) // T3DWMNF1
#define T3D_RUNTIME_WORLD_MANIFEST_VERSION		  ( 5u )
#define T3D_RUNTIME_WORLD_ADJ_N					  ( 1u << 0u )
#define T3D_RUNTIME_WORLD_ADJ_NE				  ( 1u << 1u )
#define T3D_RUNTIME_WORLD_ADJ_E					  ( 1u << 2u )
#define T3D_RUNTIME_WORLD_ADJ_SE				  ( 1u << 3u )
#define T3D_RUNTIME_WORLD_ADJ_S					  ( 1u << 4u )
#define T3D_RUNTIME_WORLD_ADJ_SW				  ( 1u << 5u )
#define T3D_RUNTIME_WORLD_ADJ_W					  ( 1u << 6u )
#define T3D_RUNTIME_WORLD_ADJ_NW				  ( 1u << 7u )
#define T3D_HLOD_RUNTIME_MAGIC					  ( 0x32444c48u ) // HLD2
#define T3D_HLOD_RUNTIME_VERSION				  ( 2u )
#define T3D_INSTANCE_CACHE_MAX_PAGES			  ( 64 )
#define T3D_INSTANCE_CACHE_MIB					  ( 1024u * 1024u )
#define T3D_ACTIVE_PHYSICAL_PAGE_COUNT			  ( 32 )
#define T3D_ACTIVE_INVALID_PAGE					  ( -1 )
#define T3D_ARENA_CACHELINE_ALIGNMENT			  ( 64u )
#define T3D_ARENA_DEFAULT_BASE_ALIGNMENT		  ( 64u )
#define T3D_RUNTIME_ALLOCATOR_BUDGET_DIVISOR	  ( 8u )
#define T3D_RUNTIME_ALLOCATOR_MIN_BYTES			  ( 4u * 1024u * 1024u )
#define T3D_RUNTIME_ALLOCATOR_MAX_BYTES			  ( 64u * 1024u * 1024u )
#define T3D_SMALL_CLASS_COUNT					  ( 9u )
#define T3D_SMALL_PAGE_BYTES					  ( 64u * 1024u )
#define T3D_MAX_EXTENTS							  ( 8192u )
#define T3D_MAX_SMALL_PAGES						  ( 1024u )
#define T3D_ALLOCATION_MAGIC					  ( 0x5A4F4E453344544FULL )
#define T3D_RESIDENT_SLOT_FREE					  ( 0u )
#define T3D_RESIDENT_SLOT_STAGING				  ( 1u )
#define T3D_RESIDENT_SLOT_PUBLISHED				  ( 2u )
#define T3D_RESIDENT_SLOT_RETIRING				  ( 3u )
#define T3D_ACQUIRE_GATE_CLOSED					  ( UINT32_C( 0x80000000 ) )
#define T3D_ACQUIRE_READER_MASK					  ( ~T3D_ACQUIRE_GATE_CLOSED )
#define T3D_BACKING_BYTES						  ( 512u * 1024u )
#define T3D_COLLISION_BAKED_CACHE_AXIS			  ( 8 )
#define T3D_BAKED_CACHE_CAPACITY				  ( T3D_COLLISION_BAKED_CACHE_AXIS * T3D_COLLISION_BAKED_CACHE_AXIS )
#define T3D_DYNAMIC_SCRATCH_AXIS				  ( 65 )
#define T3D_DYNAMIC_SCRATCH_SAMPLES				  ( T3D_DYNAMIC_SCRATCH_AXIS * T3D_DYNAMIC_SCRATCH_AXIS )
#define T3D_REGION_PRESENCE_SAMPLES				  ( 25 )
#define T3D_TERRAIN_3D_RD_CLIPMAP_RENDERER_INDIRECT_BUFFER_BYTES                                                       \
	( sizeof( t3dRdIndexedIndirectCommand_t ) * T3D_RD_DRAW_GROUP_COUNT )
#define T3D_MAX_VIEWS							( 4u )
#define T3D_FORWARD_PIPELINE_CAPACITY			( 64u )
#define T3D_RESOURCE_CAPACITY					( 1024u )
#define T3D_RESOURCE_HISTORY_CAPACITY			( 1024u )
#define T3D_FREED_HISTORY_CAPACITY				( 256u )
#define T3D_VERTEX_FORMAT_CAPACITY				( 64u )
#define T3D_TERRAIN_3D_RD_CULLING_COMMAND_BYTES ( sizeof( t3dRdIndexedIndirectCommand_t ) )
#define T3D_TERRAIN_3D_RD_CULLING_BUFFER_BYTES	( sizeof( t3dRdIndexedIndirectCommand_t ) * T3D_RD_DRAW_GROUP_COUNT )
#define T3D_MAX_SETS							( 32u )
#define T3D_FORWARD_BRIDGE_ABI					( 12u )
#define T3D_FORWARD_BRIDGE_MAX_VIEWS			( 2u )
#define T3D_FORWARD_BRIDGE_REQUIRED_CAPABILITIES                                                                       \
	( T3D_RD_FORWARD_CAP_DIRECT_POD | T3D_RD_FORWARD_CAP_DEPTH | T3D_RD_FORWARD_CAP_OPAQUE |                           \
	  T3D_RD_FORWARD_CAP_SHADOW | T3D_RD_FORWARD_CAP_MOTION | T3D_RD_FORWARD_CAP_TRANSPARENT |                         \
	  T3D_RD_FORWARD_CAP_CLUSTERED_LIGHTING | T3D_RD_FORWARD_CAP_DECALS | T3D_RD_FORWARD_CAP_REFLECTIONS |             \
	  T3D_RD_FORWARD_CAP_RESOURCE_GENERATIONS | T3D_RD_FORWARD_CAP_SPLIT_SHADOW_PACKET |                               \
	  T3D_RD_FORWARD_CAP_ENVIRONMENT_LIGHTING | T3D_RD_FORWARD_CAP_PREVIOUS_DEPTH_HIZ )
#define T3D_BRIDGE_ABI_VERSION								   ( T3D_FORWARD_BRIDGE_ABI )
#define T3D_MAX_COORDINATORS								   ( 64u )
#define T3D_DIRTY_CHUNK_RECORDS								   ( 64u )
#define T3D_FNV_OFFSET										   ( 14695981039346656037ull )
#define T3D_FNV_PRIME										   ( 1099511628211ull )
#define T3D_MIN_VERTEX_ARENA_BYTES							   ( 4u * 1024u * 1024u )
#define T3D_MIN_INDEX_ARENA_BYTES							   ( 2u * 1024u * 1024u )
#define T3D_TERRAIN_3D_RD_MESH_CACHE_CHUNK_BYTES			   ( 65536u )
#define T3D_DIRTY_UPLOAD_CHUNK_BYTES						   ( 65536u )
#define T3D_PIPELINE_CAPACITY								   ( 32u )
#define T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT				   ( 8u )
#define T3D_TRACKED_BATCH_SLOT_COUNT						   ( 256u )
#define T3D_TRACKED_BATCH_UPDATE_COUNT						   ( 6u )
#define T3D_FNV1A_OFFSET									   ( 14695981039346656037ull )
#define T3D_FNV1A_PRIME										   ( 1099511628211ull )
#define s_fPackedIntegerTolerance							   ( 0.000001 )
#define T3D_RD_MATERIAL_LAYER_COUNT							   ( 32u )
#define T3D_RD_DRAW_GROUP_COUNT								   ( 4u )
#define T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT					   ( 3u )
#define T3D_RD_INVALID_FRAME_SNAPSHOT_INDEX					   ( T3D_RD_FRAME_SNAPSHOT_SLOT_COUNT )
#define T3D_RD_FRAME_SNAPSHOT_TOKEN_INDEX_MASK				   ( 0x3u )
#define T3D_RD_FRAME_SNAPSHOT_TOKEN_PUBLISHED_MASK			   ( uint64_t( 1u ) << 63u )
#define T3D_RD_FRAME_SNAPSHOT_GENERATION_MAX				   ( ( uint64_t( 1u ) << 61u ) - 1u )
#define T3D_RD_FORWARD_BRIDGE_ABI_VERSION					   ( 12u )
#define T3D_RD_FORWARD_MAX_VIEWS							   ( 2u )
#define T3D_EDITOR_BRUSH_TEXTURE_SIZE						   ( 256 )
#define T3D_FORWARD_VIEWPORT_SLOT_COUNT						   ( 8u )
#define T3D_DERIVED_READBACK_SLOT_COUNT						   ( 3u )
#define T3D_SHADER_PACK_MAGIC								   ( 0x314b504853443354ull ) // T3DSHPK1
#define T3D_SHADER_PACK_VERSION								   ( 1u )
#define T3D_CPU_REQUEST_CAPACITY							   ( 25 )
#define T3D_ACTIVE_RADIUS									   ( 2 )
#define T3D_GPU_CACHE_DEFAULT_GUARD_REGIONS					   ( 1 )
#define T3D_GPU_CACHE_DEFAULT_HYSTERESIS_RESERVE			   ( 4 )
#define T3D_GPU_CACHE_DEFAULT_HYSTERESIS_TICKS				   ( 30 )
#define T3D_GPU_CACHE_DEFAULT_PREFETCH_ROWS					   ( 1 )
#define T3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES					   ( 4 )
#define T3D_GPU_CACHE_DEFAULT_SPARE_LAYERS					   ( 12 )
#define T3D_FAR_VT_MAX_SOURCE_HEIGHT_SIDE						   ( 512 )
#define T3D_FAR_VT_MAX_SOURCE_SURFACE_SIDE					   ( 4096 )
#define T3D_GPU_CACHE_RUNTIME_MAX_LAYERS					   ( 256 )
#define T3D_GPU_CACHE_EDITOR_MAX_LAYERS						   ( 256 )
#define T3D_GPU_CACHE_MAX_LAYERS							   ( T3D_GPU_CACHE_EDITOR_MAX_LAYERS )
#define T3D_GPU_CACHE_MAX_PAGE_TABLE_BYTES					   ( 64u * 64u * 3u )
#define T3D_GPU_CACHE_MIB									   ( 1024u * 1024u )
#define T3D_GPU_CACHE_MIN_SPARE_LAYERS						   ( 2 )
#define T3D_DISCOVERY_BYTES									   ( 12u )
#define T3D_RUNTIME_WORKER_LIMIT							   ( 32 )
#define T3D_TERRAIN_3D_STREAMING_INTEGRATION_BOUNDARY_FRACTION ( 0.25 )
#define T3D_NATIVE_API_REVISION								   ( 2026091003LL )

// File Paths (e.g. extras)
#define OCEAN_SETTINGS_PATH "res://addons/terrain_3d/extras/shaders/M_ocean.tres"

// Engine Shortcuts
#define PS		  PhysicsServer3D::get_singleton()
#define IS_EDITOR Engine::get_singleton()->is_editor_hint()

// Constants

// Centralized native compile-time constants. Keep tunable limits and format constants here.

// Full-region heightmap collision samples region_size + 1 vertices. Keep the
// axis scratch large enough for the maximum supported 2048-texel region.

#define s_regionMapVSize ( Vector2i( T3D_REGION_MAP_SIZE, T3D_REGION_MAP_SIZE ) )

// End centralized native compile-time constants.

#define s_colorNan	 ( Color( NAN, NAN, NAN, NAN ) )
#define s_colorWhite ( Color( 1.0f, 1.0f, 1.0f, 1.0f ) )
static const Image::Format s_szRegionMapFormats[T3D_TYPE_MAX_VALUE + 1u] = {
	Image::FORMAT_R16,
	Image::FORMAT_RF,
	Image::FORMAT_RGBA8,
	Image::Format( T3D_TYPE_MAX_VALUE ),
};

#define COLOR_BLACK		Color( 0.0f, 0.0f, 0.0f, 1.0f )
#define COLOR_ROUGHNESS Color( 1.0f, 1.0f, 1.0f, 0.5f )
#define COLOR_CHECKED	Color( 1.f, 1.f, 1.0f, -1.0f )
#define COLOR_NORMAL	Color( 0.5f, 0.5f, 1.0f, 1.0f )

#define COLOR_CONTROL Color( s_fControlAutoFloat, 0.f, 0.f, 1.0f )
#define s_szRegionMapDefaultColors( nMapType )                                                                         \
	( ( nMapType ) == T3D_TYPE_HEIGHT_VALUE                                                                            \
		  ? COLOR_BLACK                                                                                                \
		  : ( ( nMapType ) == T3D_TYPE_CONTROL_VALUE                                                                   \
				  ? COLOR_CONTROL                                                                                      \
				  : ( ( nMapType ) == T3D_TYPE_COLOR_VALUE ? COLOR_ROUGHNESS : s_colorNan ) ) )

// For consistency between MSVC, gcc, clang
#ifndef FLT_MAX
#define FLT_MAX __FLT_MAX__
#endif

// Warning flags are stored in an 8-bit mask.
#define WARN_MISMATCHED_SIZE	0x01
#define WARN_MISMATCHED_FORMAT	0x02
#define WARN_MISMATCHED_MIPMAPS 0x04
#define WARN_ALL				0xFF

// Global Types

#define V2( x )	  Vector2( x, x )
#define V2I( x )  Vector2i( x, x )
#define V1( x )	  Vector3( x, x, x )
#define s_v2Zero  ( Vector2( 0.f, 0.f ) )
#define s_v2Max	  ( Vector2( FLT_MAX, FLT_MAX ) )
#define s_v2iZero ( Vector2i( 0, 0 ) )
#define s_v2iMax  ( Vector2i( INT32_MAX, INT32_MAX ) )

#define T3D_REGION_COORD_INVALID ( INT8_MAX )

#define s_v3Zero ( Vector3( 0.f, 0.f, 0.f ) )
#define s_v3Max	 ( Vector3( FLT_MAX, FLT_MAX, FLT_MAX ) )
#define s_v3Nan	 ( Vector3( NAN, NAN, NAN ) )
#define s_v3Up	 ( Vector3( 0.f, 1.f, 0.f ) )
// static const Vector3 V1_DOWN{ 0.f, -1.f, 0.f };

// Set class name for logger.h

#define CLASS_NAME()                                                                                                   \
	const String __class__ = get_class_static() + String( "#" ) + String::num_uint64( get_instance_id() ).right( 4 );

#define CLASS_NAME_STATIC( pszName ) static inline const char *__class__ = pszName;

// Validation macros

#define ASSERT( cond, ret )                                                                                            \
	if ( !( cond ) ) {                                                                                                 \
		UtilityFunctions::push_error( "Assertion '", #cond, "' failed at ", __FILE__, ":", __LINE__ );                 \
		return ret;                                                                                                    \
	}

#define VOID // a return value for void, to avoid compiler warnings

#define IS_INIT( ret )                                                                                                 \
	if ( !m_pTerrain ) {                                                                                               \
		return ret;                                                                                                    \
	}

#define IS_INIT_MESG( mesg, ret )                                                                                      \
	if ( !m_pTerrain ) {                                                                                               \
		LOG( ERROR, mesg );                                                                                            \
		return ret;                                                                                                    \
	}

#define IS_INIT_COND( cond, ret )                                                                                      \
	if ( !m_pTerrain || cond ) {                                                                                       \
		return ret;                                                                                                    \
	}

#define IS_INIT_COND_MESG( cond, mesg, ret )                                                                           \
	if ( !m_pTerrain || cond ) {                                                                                       \
		LOG( ERROR, mesg );                                                                                            \
		return ret;                                                                                                    \
	}

#define IS_INSTANCER_INIT( ret )                                                                                       \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) {                                                                \
		return ret;                                                                                                    \
	}

#define IS_INSTANCER_INIT_MESG( mesg, ret )                                                                            \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) {                                                                \
		LOG( ERROR, mesg );                                                                                            \
		return ret;                                                                                                    \
	}

#define IS_DATA_INIT( ret )                                                                                            \
	if ( !m_pTerrain || !m_pTerrain->getData() ) {                                                                     \
		return ret;                                                                                                    \
	}

#define IS_DATA_INIT_MESG( mesg, ret )                                                                                 \
	if ( !m_pTerrain || !m_pTerrain->getData() ) {                                                                     \
		LOG( ERROR, mesg );                                                                                            \
		return ret;                                                                                                    \
	}

// Additional migrated constants.
#define s_pszNativeBuildId		 ( "mesh-derived-instance-physics-occlusion-p13-2-rd-capacity-align-2026-09-16" )
#define s_pszEngineSingletonName ( "Terrain3DForwardPlusBridge" )
#define s_pszProfileKey			 ( "xbox360_experimental" )
#define s_pszProfileDisplayName	 ( "Xbox 360 Experimental" )
static const char *const g_t3dConstStorage_s_pszCacheThrashSubsystemNames[] = {
	"gpu_terrain_pages",
	"cpu_terrain_pages",
	"instance_pages",
};
#define s_pszCacheThrashSubsystemNames g_t3dConstStorage_s_pszCacheThrashSubsystemNames
static const char *const g_t3dConstStorage_s_pszMemorySubsystemNames[] = {
	"gpu_terrain_pages",
	"cpu_terrain_pages",
	"instance_pages",
	"baked_lighting",
	"hlod_proxies",
	"shader_cache",
};
#define s_pszMemorySubsystemNames g_t3dConstStorage_s_pszMemorySubsystemNames
static const char *const g_t3dConstStorage_s_pszEditorToolNames[] = {
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
#define s_pszEditorToolNames g_t3dConstStorage_s_pszEditorToolNames
static const char *const g_t3dConstStorage_s_pszEditorOperationNames[] = {
	"Add",
	"Subtract",
	"Replace",
	"Average",
	"Gradient",
	"OP_MAX",
};
#define s_pszEditorOperationNames g_t3dConstStorage_s_pszEditorOperationNames
static const char *const g_t3dConstStorage_s_pszShaderStaticMacroNames[] = {
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
#define s_pszShaderStaticMacroNames g_t3dConstStorage_s_pszShaderStaticMacroNames
static const char *const g_t3dConstStorage_s_pszRegionMapTypeNames[T3D_TYPE_MAX_VALUE + 1u] = {
	"TYPE_HEIGHT",
	"TYPE_CONTROL",
	"TYPE_COLOR",
	"TYPE_MAX",
};
#define s_pszRegionMapTypeNames g_t3dConstStorage_s_pszRegionMapTypeNames
static const char *const g_t3dConstStorage_s_pszShaderFeatures[] = {
	"AUTO_SHADER",
	"AUTO_SHADER_UNIFORMS",
	"BLEND_WEIGHT_FUNCTIONS",
	"CLIPMAP_BATCHING_FUNCTIONS",
	"DISPLACEMENT_FUNCTIONS",
	"DISPLACEMENT_UNIFORMS",
	"DISPLACEMENT_VERTEX",
	"DUAL_SCALING",
	"DUAL_SCALING_CONDITION_0",
	"DUAL_SCALING_CONDITION_1",
	"DUAL_SCALING_FACTOR",
	"DUAL_SCALING_MIX",
	"DUAL_SCALING_UNIFORMS",
	"FAR_FLAT_FUNCTIONS",
	"FAR_NONE_FRAGMENT",
	"FAR_NONE_FUNCTIONS",
	"FLAT_FRAGMENT",
	"FLAT_FUNCTIONS",
	"FLAT_UNIFORMS",
	"FLAT_VERTEX",
	"LEGACY_PBR_DISPLACEMENT_FUNCTIONS",
	"LEGACY_PBR_MATERIAL_FUNCTIONS",
	"LEGACY_PBR_UNIFORMS",
	"MACRO_VARIATION",
	"MACRO_VARIATION_UNIFORMS",
	"MIDDLE_NONE_FRAGMENT",
	"NONE_CHECK",
	"NONE_FUNCTIONS",
	"OUTPUT_ALBEDO",
	"OUTPUT_ALBEDO_GREY",
	"OUTPUT_AMBIENT_OCCLUSION",
	"OUTPUT_METALLIC",
	"OUTPUT_NORMAL_MAP",
	"OUTPUT_ROUGHNESS",
	"OUTPUT_SPECULAR",
	"OUTPUT_SPECULAR_NONE",
	"PROJECTION",
	"SURFACE_DISPLACEMENT_UNIFORM",
	"SURFACE_PBR_CORE_FUNCTIONS",
	"SURFACE_PBR_DISPLACEMENT_FUNCTIONS",
	"SURFACE_PBR_MATERIAL_FUNCTIONS",
	"SURFACE_PBR_UNIFORMS",
	"TERRAIN_MATERIAL_SAMPLING",
	"TEXTURE_SAMPLERS_LINEAR",
	"TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC",
	"TEXTURE_SAMPLERS_NEAREST",
	"TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC",
	"WORLD_NOISE_FRAGMENT",
	"WORLD_NOISE_FUNCTIONS",
	"WORLD_NOISE_UNIFORMS",
	"WORLD_NOISE_VERTEX",
};
#define s_pszShaderFeatures g_t3dConstStorage_s_pszShaderFeatures
static const uint32_t g_t3dConstStorage_s_szSmallClasses[T3D_SMALL_CLASS_COUNT] =
	{ 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u };
#define s_szSmallClasses g_t3dConstStorage_s_szSmallClasses

#endif // TERRAIN3D_CONSTANTS_H
