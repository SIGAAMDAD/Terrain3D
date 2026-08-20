// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_CONSTANTS_H
#define TERRAIN3D_CONSTANTS_H

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <limits>

#include "core/terrain_3d_numeric_constants.h"
#include "data/terrain_3d_runtime_page_constants.h"
#include "instances/terrain_3d_instance_cache_constants.h"
#include "streaming/terrain_3d_gpu_cache_constants.h"
#include "memory/terrain_3d_memory_constants.h"

#include <EASTL/functional.h>
#include <EASTL/utility.h>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

// GDExtension uses the godot namespace, custom modules do not.
#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// File Paths (e.g. extras)
#define OCEAN_MATERIAL_PATH "res://addons/terrain_3d/extras/shaders/M_ocean.tres"

// Engine Shortcuts
#define RS RenderingServer::get_singleton()
#define PS PhysicsServer3D::get_singleton()
#define IS_EDITOR Engine::get_singleton()->is_editor_hint()

// Constants

// Centralized native compile-time constants. Keep tunable limits and format constants here.
static constexpr uint32_t TERRAIN3D_PROFILE_VERSION = 5u;
static constexpr const char *TERRAIN3D_PROFILE_KEY = "xbox360_experimental";
static constexpr const char *TERRAIN3D_PROFILE_DISPLAY_NAME = "Xbox 360 Experimental";
static constexpr uint64_t TERRAIN3D_PROFILE_FNV_OFFSET = 1469598103934665603ull;
static constexpr uint64_t TERRAIN3D_PROFILE_FNV_PRIME = 1099511628211ull;
static constexpr uint32_t TERRAIN3D_HASH_GOLDEN_RATIO_32 = 0x9e3779b9u;
static constexpr uint32_t TERRAIN3D_HASH_MIX_32 = 0x85ebca6bu;
static constexpr uint32_t TERRAIN3D_HASH_MIX_32_SECONDARY = 0xc2b2ae35u;
static constexpr uint64_t TERRAIN3D_HASH_GOLDEN_RATIO_64 = 0x9e3779b97f4a7c15ULL;
static constexpr float TERRAIN3D_UINT32_MAX_FLOAT = 4294967295.0f;
static constexpr size_t TERRAIN3D_TEST_HALF_ENCODING_COUNT = 65536u;
static constexpr size_t TERRAIN3D_TEST_TRANSFORM_COUNT = 37u;
static constexpr int TERRAIN3D_TEST_NORMAL_WIDTH = 37;
static constexpr int TERRAIN3D_TEST_NORMAL_HEIGHT = 19;
static constexpr int TERRAIN3D_TEST_NORMAL_STRIDE = TERRAIN3D_TEST_NORMAL_WIDTH + 2;
static constexpr size_t TERRAIN3D_TEST_EROSION_COUNT = 4099u;
static constexpr size_t TERRAIN3D_TEST_NON_FINITE_EROSION_COUNT = 16u;
static constexpr size_t TERRAIN3D_TEST_DISPATCH_COUNT = 1024u;
static constexpr size_t TERRAIN3D_TEST_DISPATCH_ROUNDS = 32u;
static constexpr size_t TERRAIN3D_TEST_DISPATCH_ITERATIONS = 64u;
static constexpr size_t TERRAIN3D_TEST_EASTL_ALIGNMENT = 64u;
static constexpr size_t TERRAIN3D_TEST_EASTL_OFFSET = 13u;
static constexpr uint8_t TERRAIN3D_TEST_CONTROL_FLAG = 1u << 0u;
static constexpr const char *TERRAIN3D_CACHE_THRASH_SUBSYSTEM_NAMES[] = {
	"gpu_terrain_pages", "cpu_terrain_pages", "instance_pages",
};
static constexpr const char *TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[] = {
	"gpu_terrain_pages", "cpu_terrain_pages", "instance_pages", "baked_lighting", "hlod_proxies", "shader_cache",
};
static constexpr size_t TERRAIN3D_MEMORY_SUBSYSTEM_NAME_COUNT =
	sizeof( TERRAIN3D_MEMORY_SUBSYSTEM_NAMES ) / sizeof( TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[0] );
static constexpr const char *TERRAIN3D_EDITOR_TOOL_NAMES[] = {
	"Region", "Sculpt", "Height", "Texture", "Color", "Roughness", "Auto Shader",
	"Holes", "Navigation", "Instancer", "Angle", "Scale", "Landform", "TOOL_MAX",
};
static constexpr const char *TERRAIN3D_EDITOR_OPERATION_NAMES[] = {
	"Add", "Subtract", "Replace", "Average", "Gradient", "OP_MAX",
};
static constexpr const char *TERRAIN3D_SHADER_STATIC_MACRO_NAMES[] = {
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
static constexpr RenderingServer::ShadowCastingSetting TERRAIN3D_SHADOW_CASTING_ON =
	RenderingServer::SHADOW_CASTING_SETTING_ON;
static constexpr RenderingServer::ShadowCastingSetting TERRAIN3D_SHADOW_CASTING_OFF =
	RenderingServer::SHADOW_CASTING_SETTING_OFF;
static constexpr RenderingServer::ShadowCastingSetting TERRAIN3D_SHADOW_CASTING_ONLY =
	RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY;
static constexpr int TERRAIN3D_MAX_TEXTURE_ASSETS = 32;
static constexpr int TERRAIN3D_MAX_MESH_ASSETS = 256;
static constexpr real_t TERRAIN3D_DATA_CURRENT_VERSION = 0.94f;
static constexpr float TERRAIN3D_REGION_HEIGHT_MIN = -384.0f;
static constexpr float TERRAIN3D_REGION_HEIGHT_MAX = 768.0f;
static constexpr float TERRAIN3D_REGION_HEIGHT_RANGE = TERRAIN3D_REGION_HEIGHT_MAX - TERRAIN3D_REGION_HEIGHT_MIN;
static constexpr float TERRAIN3D_REGION_HEIGHT_INV_RANGE = 1.0f / TERRAIN3D_REGION_HEIGHT_RANGE;
static constexpr float TERRAIN3D_REGION_HEIGHT_U16_SCALE = 65535.0f;
static constexpr uint32_t TERRAIN3D_REGION_DEFAULT_CONTROL = 1u;
static constexpr uint32_t TERRAIN3D_AUTOSHADER_MATERIAL_MASK = ( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u );
static constexpr float TERRAIN3D_DEGREES_PER_RADIAN = 57.29577951308232f;
static constexpr float TERRAIN3D_SQRT_TWO = 1.4142135623730951f;
static constexpr float TERRAIN3D_LARGE_DISTANCE = 1.0e20f;
static constexpr uint32_t TERRAIN3D_LANDFORM_MATERIAL_MASK = TERRAIN3D_AUTOSHADER_MATERIAL_MASK | 0x1u;
static constexpr float TERRAIN3D_LANDFORM_EPSILON = 0.0001f;
static constexpr int32_t TERRAIN3D_EROSION_DIRECTION_COUNT = 4;
static constexpr int32_t TERRAIN3D_EROSION_DIRECTION_X[TERRAIN3D_EROSION_DIRECTION_COUNT] = {-1, 1, 0, 0};
static constexpr int32_t TERRAIN3D_EROSION_DIRECTION_Y[TERRAIN3D_EROSION_DIRECTION_COUNT] = {0, 0, -1, 1};
static constexpr int32_t TERRAIN3D_EROSION_OPPOSITE_DIRECTION[TERRAIN3D_EROSION_DIRECTION_COUNT] = {1, 0, 3, 2};
static constexpr float TERRAIN3D_EROSION_EPSILON = 1.0e-6f;
static constexpr uint64_t TERRAIN3D_EROSION_FNV_OFFSET = 1469598103934665603ULL;
static constexpr uint64_t TERRAIN3D_EROSION_FNV_PRIME = 1099511628211ULL;
static constexpr size_t TERRAIN3D_EROSION_HALF_DECODE_BLOCK = 256u;
static constexpr float TERRAIN3D_CLIPMAP_PART_STATIC = 0.0f;
static constexpr float TERRAIN3D_CLIPMAP_PART_EDGE_A0 = 1.0f;
static constexpr float TERRAIN3D_CLIPMAP_PART_EDGE_A1 = 2.0f;
static constexpr float TERRAIN3D_CLIPMAP_PART_EDGE_B0 = 3.0f;
static constexpr float TERRAIN3D_CLIPMAP_PART_EDGE_B1 = 4.0f;
static constexpr int TERRAIN3D_MULTIMESH_STRIDE_FLOATS = 16;
static constexpr uint32_t TERRAIN3D_COMPUTE_WORKGROUP_SIZE = 64u;
static constexpr int TERRAIN3D_SHADER_MAX_CONDITIONAL_DEPTH = 32;
static constexpr uint32_t TERRAIN3D_INSTANCE_PAGE_MAGIC = 0x49334454u;
static constexpr uint16_t TERRAIN3D_INSTANCE_PAGE_VERSION = 2u;
static constexpr uint16_t TERRAIN3D_INSTANCE_PAGE_LEGACY_VERSION = 1u;
static constexpr size_t TERRAIN3D_INSTANCE_PAGE_HEADER_BYTES = 16u;
static constexpr size_t TERRAIN3D_INSTANCE_PAGE_MESH_HEADER_BYTES = 12u;
static constexpr size_t TERRAIN3D_INSTANCE_PAGE_LEGACY_MESH_HEADER_BYTES = 8u;
static constexpr size_t TERRAIN3D_INSTANCE_PAGE_CELL_HEADER_BYTES = 12u;
static constexpr uint16_t TERRAIN3D_INSTANCE_PAGE_CELL_COLORS = 1u << 0u;
static constexpr uint16_t TERRAIN3D_INSTANCE_PAGE_CELL_OWNERS = 1u << 1u;
static constexpr uint16_t TERRAIN3D_INSTANCE_PAGE_CELL_FLAGS = TERRAIN3D_INSTANCE_PAGE_CELL_COLORS | TERRAIN3D_INSTANCE_PAGE_CELL_OWNERS;
static constexpr uint32_t TERRAIN3D_ACTIVE_ACCESS_READER_MASK = 0x7fffffffu;
static constexpr uint32_t TERRAIN3D_ACTIVE_ACCESS_WRITER_BIT = 0x80000000u;
static constexpr size_t TERRAIN3D_ACTIVE_STORAGE_ALIGNMENT = 64u;
static constexpr int TERRAIN3D_AUTOSHADER_MODE_LEGACY_RUNTIME = 0;
static constexpr int TERRAIN3D_AUTOSHADER_MODE_BAKED = 1;
static constexpr int TERRAIN3D_AUTOSHADER_MODE_DISABLED = 2;
static constexpr int TERRAIN3D_AUTOSHADER_MODE_EDITOR_PREVIEW = 3;
static constexpr size_t TERRAIN3D_BAKE_CACHELINE_BYTES = 64u;
static constexpr size_t TERRAIN3D_COMPUTE_SCRATCH_RETAIN_LIMIT = 32u * 1024u * 1024u;
static constexpr size_t TERRAIN3D_CPU_CACHE_BYTES_PER_TEXEL = 12u;
static constexpr int TERRAIN3D_CPU_CACHE_MAX_LAYERS = 64;
static constexpr size_t TERRAIN3D_CPU_CACHE_MIB = 1024u * 1024u;
static constexpr int TERRAIN3D_CPU_CACHE_MIN_SAMPLE_LAYERS = 4;
static constexpr int TERRAIN3D_CPU_REGION_CACHE_MAX_BUDGET_MB = 128;
static constexpr int TERRAIN3D_CPU_REGION_CACHE_MIN_BUDGET_MB = 1;
static constexpr int TERRAIN3D_CPU_REGION_LOAD_MAX_BUDGET_MB = 64;
static constexpr int TERRAIN3D_CPU_REGION_LOAD_MAX_LAYERS = 64;
static constexpr size_t TERRAIN3D_DEFAULT_RESIDENT_POOL_BUDGET = 64u * 1024u * 1024u;
static constexpr int TERRAIN3D_DIAGNOSTICS_VERSION = 1;
static constexpr uint32_t TERRAIN3D_DIRECTIONAL_RECEIVER_LAYER = 1u << 19u;
static constexpr int32_t TERRAIN3D_EROSION_REGION_LOOKUP_CAPACITY = 4096;
static constexpr int32_t TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT = 32;
static constexpr uint32_t TERRAIN3D_FEATURE_ALT_VALUE = 1u << 0u;
static constexpr uint32_t TERRAIN3D_FEATURE_ANGLE_VALUE = 1u << 3u;
static constexpr uint32_t TERRAIN3D_FEATURE_CENTER_VALID_VALUE = 1u << 1u;
static constexpr uint32_t TERRAIN3D_FEATURE_SCALE_VALUE = 1u << 4u;
static constexpr uint32_t TERRAIN3D_FEATURE_TEXTURE_VALUE = 1u << 2u;
static constexpr int TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS = 512;
static constexpr int TERRAIN3D_GPU_HEIGHT_CACHE_MIN_SPARE_LAYERS = 8;
static constexpr uint64_t TERRAIN3D_GPU_FAR_ATLAS_HEADER_BYTES = 52u;
static constexpr uint64_t TERRAIN3D_GPU_FAR_ATLAS_MAGIC = 0x5444334641523031ull;
static constexpr uint32_t TERRAIN3D_GPU_FAR_ATLAS_VERSION = 7u;
static constexpr int TERRAIN3D_GPU_REGION_CACHE_MAX_BUDGET_MB = 128;
static constexpr int TERRAIN3D_GPU_REGION_CACHE_MIN_BUDGET_MB = 1;
static constexpr int TERRAIN3D_GPU_REGION_UPLOAD_MAX_BUDGET_MB = 64;
static constexpr int TERRAIN3D_GPU_REGION_UPLOAD_MAX_LAYERS = 64;
static constexpr int TERRAIN3D_INSTANCE_DEFAULT_CELL_SIZE = 32;
static constexpr int TERRAIN3D_INSTANCE_MAX_CELLS_PER_AXIS = 256;
static constexpr int TERRAIN3D_INSTANCE_MAX_CELL_SIZE = 128;
static constexpr int TERRAIN3D_INSTANCE_MAX_REGION_SIZE = 2048;
static constexpr int TERRAIN3D_INSTANCE_MIN_CELL_SIZE = 8;
static constexpr int TERRAIN3D_INSTANCE_MIN_REGION_SIZE = 64;
static constexpr int TERRAIN3D_INSTANCE_REGION_CACHE_MAX_BUDGET_MB = 128;
static constexpr int TERRAIN3D_INSTANCE_REGION_CACHE_MIN_BUDGET_MB = 1;
static constexpr int TERRAIN3D_INSTANCE_REGION_LOAD_MAX_BUDGET_MB = 64;
static constexpr int TERRAIN3D_INSTANCE_REGION_LOAD_MAX_PAGES = 64;
static constexpr int TERRAIN3D_INSTANCE_TRANSITION_BUCKET_CELL_SPAN = 4;
static constexpr uint32_t TERRAIN3D_INSTANCE_WHITE_RGBA8 = 0xffffffffu;
static constexpr uint32_t TERRAIN3D_LIGHTING_LAYER_FOLIAGE = 1u << 15u;
static constexpr uint32_t TERRAIN3D_LIGHTING_LAYER_LARGE = 1u << 17u;
static constexpr uint32_t TERRAIN3D_LIGHTING_LAYER_MEDIUM = 1u << 16u;
static constexpr uint32_t TERRAIN3D_LIGHTING_LAYER_REGIONAL = 1u << 18u;
static constexpr int TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT = 8;
static constexpr uint64_t TERRAIN3D_LIGHTMAP_FNV_OFFSET = 1469598103934665603ull;
static constexpr uint64_t TERRAIN3D_LIGHTMAP_FNV_PRIME = 1099511628211ull;
static constexpr int TERRAIN3D_MAX_CLIPMAP_LEVELS = 16;
static constexpr int TERRAIN3D_MAX_REGION_BLEND_SAMPLES = 2048;
// Full-region heightmap collision samples region_size + 1 vertices. Keep the
// axis scratch large enough for the maximum supported 2048-texel region.
static constexpr int TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES = TERRAIN3D_MAX_REGION_BLEND_SAMPLES + 1;
static constexpr int TERRAIN3D_MAX_RENDER_BATCHES = 5;
static constexpr int TERRAIN3D_MAX_SHADER_VARIANT_CACHE_LIMIT = 256;
static constexpr int TERRAIN3D_MAX_SHADER_VARIANT_PREWARM_LIMIT = 64;
static constexpr int TERRAIN3D_MAX_SHADER_VARIANT_SOURCE_BUDGET_MB = 256;
static constexpr int TERRAIN3D_MAX_TILE_INSTANCES = 16;
static constexpr uint64_t TERRAIN3D_MEMORY_MIB = 1024ULL * 1024ULL;
static constexpr int TERRAIN3D_MEMORY_POLICY_INTERVAL_FRAMES = 30;
static constexpr int TERRAIN3D_MESH_MAX_LOD_COUNT = 10;
static constexpr int TERRAIN3D_MESH_SHADOW_LOD_ID = -1;
static constexpr int TERRAIN3D_MIN_REGION_SIZE = 64;
static constexpr int TERRAIN3D_MIN_SHADER_VARIANT_CACHE_LIMIT = 8;
static constexpr int TERRAIN3D_MIN_SHADER_VARIANT_PREWARM_LIMIT = 1;
static constexpr int TERRAIN3D_MIN_SHADER_VARIANT_SOURCE_BUDGET_MB = 1;
static constexpr uint32_t TERRAIN3D_OP_ADD_VALUE = 0u;
static constexpr uint32_t TERRAIN3D_OP_GRADIENT_VALUE = 4u;
static constexpr uint32_t TERRAIN3D_OP_REPLACE_VALUE = 2u;
static constexpr uint32_t TERRAIN3D_OP_SUBTRACT_VALUE = 1u;
static constexpr int TERRAIN3D_PAIR_INSTANCES = 2;
static constexpr int TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS = 2;
static constexpr int TERRAIN3D_RENDER_CONTROL_KERNEL[5] = {1, 2, 3, 2, 1};
static constexpr uint32_t TERRAIN3D_RENDER_CONTROL_MATERIAL_MASK =
	( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u );
static constexpr size_t TERRAIN3D_RESIDENT_CACHE_ALIGNMENT = 64u;
static constexpr size_t TERRAIN3D_RESIDENT_INVALID_OFFSET = ( size_t ) ( -1 );
static constexpr int TERRAIN3D_RING_TILE_INSTANCES = 12;
static constexpr size_t TERRAIN3D_RUNTIME_PROFILE_HISTORY_CAPACITY = 600u;
static constexpr int TERRAIN3D_SAMPLER_CACHE_SIZE = 8;
static constexpr uint32_t TERRAIN3D_TOOL_AUTOSHADER_VALUE = 6u;
static constexpr uint32_t TERRAIN3D_TOOL_COLOR_VALUE = 4u;
static constexpr uint32_t TERRAIN3D_TOOL_HEIGHT_VALUE = 2u;
static constexpr uint32_t TERRAIN3D_TOOL_HOLES_VALUE = 7u;
static constexpr uint32_t TERRAIN3D_TOOL_NAVIGATION_VALUE = 8u;
static constexpr uint32_t TERRAIN3D_TOOL_ROUGHNESS_VALUE = 5u;
static constexpr uint32_t TERRAIN3D_TOOL_TEXTURE_VALUE = 3u;
static constexpr uint32_t TERRAIN3D_TYPE_COLOR_VALUE = 2u;
static constexpr uint32_t TERRAIN3D_TYPE_CONTROL_VALUE = 1u;
static constexpr uint32_t TERRAIN3D_TYPE_HEIGHT_VALUE = 0u;
static constexpr uint32_t TERRAIN3D_TYPE_MAX_VALUE = 3u;
static constexpr int TERRAIN3D_WORLD_TEXELS = 4096;
static constexpr size_t TERRAIN3D_XBOX360_CPU_MAP_CEILING_BYTES = 64u * 1024u * 1024u;
static constexpr int TERRAIN3D_ACTIVE_MIN_REGION_SIZE = TERRAIN3D_MIN_REGION_SIZE;
static constexpr int TERRAIN3D_ACTIVE_WORLD_TEXELS = TERRAIN3D_WORLD_TEXELS;
static constexpr size_t TERRAIN3D_BAKE_UV_CACHELINE_ELEMENTS =
	TERRAIN3D_BAKE_CACHELINE_BYTES / std::gcd( TERRAIN3D_BAKE_CACHELINE_BYTES, sizeof( Vector2 ) );
static constexpr size_t TERRAIN3D_BAKE_VERTEX_CACHELINE_ELEMENTS =
	TERRAIN3D_BAKE_CACHELINE_BYTES / std::gcd( TERRAIN3D_BAKE_CACHELINE_BYTES, sizeof( Vector3 ) );
static constexpr size_t TERRAIN3D_GPU_CACHE_FAR_COLOR_BYTES =
	( size_t ) ( TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE ) * ( size_t ) ( TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE ) * 4u;
static constexpr size_t TERRAIN3D_GPU_CACHE_FAR_HEIGHT_BYTES =
	( size_t ) ( TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE ) * ( size_t ) ( TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE ) * 2u;
static constexpr int TERRAIN3D_GPU_FAR_HEIGHT_SIZE = TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
static constexpr uint32_t TERRAIN3D_LIGHTING_CLASS_LAYER_MASK =
	TERRAIN3D_LIGHTING_LAYER_FOLIAGE | TERRAIN3D_LIGHTING_LAYER_MEDIUM |
	TERRAIN3D_LIGHTING_LAYER_LARGE | TERRAIN3D_LIGHTING_LAYER_REGIONAL;
static constexpr int TERRAIN3D_MAX_BATCH_INSTANCES = TERRAIN3D_MAX_CLIPMAP_LEVELS;
static constexpr int TERRAIN3D_MAX_BUFFER_SHADER_VARIANT_CACHE_LIMIT = TERRAIN3D_MAX_SHADER_VARIANT_CACHE_LIMIT / 2;
static constexpr int TERRAIN3D_MAX_EDITOR_SHADER_VARIANT_CACHE_LIMIT = TERRAIN3D_MAX_SHADER_VARIANT_CACHE_LIMIT / 4;
static constexpr int TERRAIN3D_MAX_REGION_MAP_SIZE =
	TERRAIN3D_WORLD_TEXELS / TERRAIN3D_MIN_REGION_SIZE;
static constexpr uint64_t TERRAIN3D_MEMORY_POLICY_HYSTERESIS_BYTES = 2ULL * TERRAIN3D_MEMORY_MIB;
static constexpr int TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE = TERRAIN3D_MAX_REGION_MAP_SIZE;
static const Vector2i TERRAIN3D_REGION_MAP_VSIZE{TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE, TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE};
static constexpr size_t TERRAIN3D_BAKE_TASK_ALIGNMENT_ELEMENTS =
	std::lcm( TERRAIN3D_BAKE_VERTEX_CACHELINE_ELEMENTS, TERRAIN3D_BAKE_UV_CACHELINE_ELEMENTS );
static constexpr uint32_t TERRAIN3D_LIGHTING_MANAGED_LAYER_MASK =
	TERRAIN3D_LIGHTING_CLASS_LAYER_MASK | TERRAIN3D_DIRECTIONAL_RECEIVER_LAYER;
static constexpr int TERRAIN3D_MAX_REGION_SLOTS =
	TERRAIN3D_MAX_REGION_MAP_SIZE * TERRAIN3D_MAX_REGION_MAP_SIZE;
static constexpr int TERRAIN3D_RESIDENT_WORLD_SIZE = TERRAIN3D_MAX_REGION_MAP_SIZE;
static constexpr int TERRAIN3D_ACTIVE_MAX_REGION_SLOTS = TERRAIN3D_MAX_REGION_SLOTS;
static constexpr size_t TERRAIN3D_INSTANCE_REGION_TABLE_CAPACITY = ( size_t ) ( TERRAIN3D_MAX_REGION_SLOTS );
static constexpr int TERRAIN3D_RESIDENT_WORLD_HALF = TERRAIN3D_RESIDENT_WORLD_SIZE / 2;
static constexpr int TERRAIN3D_RESIDENT_WORLD_SLOT_COUNT = TERRAIN3D_MAX_REGION_SLOTS;
static constexpr size_t TERRAIN3D_INSTANCE_REGION_UPDATE_CAPACITY = TERRAIN3D_INSTANCE_REGION_TABLE_CAPACITY;
namespace terrain_3d_profile {

typedef enum t3downer_e : uint8_t {
	OWNER_TERRAIN,
	OWNER_MATERIAL,
} t3downer_t;

typedef enum t3dvalueKind_e : uint8_t {
	VALUE_INT,
	VALUE_FLOAT,
	VALUE_BOOL,
} t3dvalueKind_t;

typedef struct t3dsetting_s {
	t3downer_t owner;
	t3dvalueKind_t kind;
	const char *pszProperty;
	int64_t intValue;
	double fFloatValue;
	bool bBoolValue;
} t3dsetting_t;

#define T3D_PROFILE_TERRAIN_INT( name, value ) { OWNER_TERRAIN, VALUE_INT, name, ( int64_t ) ( value ), 0.0, false },
#define T3D_PROFILE_TERRAIN_FLOAT( name, value ) { OWNER_TERRAIN, VALUE_FLOAT, name, 0, ( double ) ( value ), false },
#define T3D_PROFILE_TERRAIN_BOOL( name, value ) { OWNER_TERRAIN, VALUE_BOOL, name, 0, 0.0, ( bool ) ( value ) },
#define T3D_PROFILE_MATERIAL_INT( name, value ) { OWNER_MATERIAL, VALUE_INT, name, ( int64_t ) ( value ), 0.0, false },
#define T3D_PROFILE_MATERIAL_FLOAT( name, value ) { OWNER_MATERIAL, VALUE_FLOAT, name, 0, ( double ) ( value ), false },
#define T3D_PROFILE_MATERIAL_BOOL( name, value ) { OWNER_MATERIAL, VALUE_BOOL, name, 0, 0.0, ( bool ) ( value ) },

inline constexpr t3dsetting_t XBOX360_EXPERIMENTAL_SETTINGS[] = {
#include "streaming/terrain_3d_performance_profile_manifest.inc"
};
inline constexpr size_t XBOX360_EXPERIMENTAL_SETTING_COUNT =
	sizeof( XBOX360_EXPERIMENTAL_SETTINGS ) / sizeof( XBOX360_EXPERIMENTAL_SETTINGS[0] );

#undef T3D_PROFILE_TERRAIN_INT
#undef T3D_PROFILE_TERRAIN_FLOAT
#undef T3D_PROFILE_TERRAIN_BOOL
#undef T3D_PROFILE_MATERIAL_INT
#undef T3D_PROFILE_MATERIAL_FLOAT
#undef T3D_PROFILE_MATERIAL_BOOL

} // namespace terrain_3d_profile

// End centralized native compile-time constants.

static const Color COLOR_NAN{NAN, NAN, NAN, NAN};
static const Color COLOR_WHITE{1.0f, 1.0f, 1.0f, 1.0f};
static const Image::Format TERRAIN3D_REGION_MAP_FORMATS[TERRAIN3D_TYPE_MAX_VALUE + 1u] = {
	Image::FORMAT_R16,
	Image::FORMAT_RF,
	Image::FORMAT_RGBA8,
	Image::Format( TERRAIN3D_TYPE_MAX_VALUE ),
};
static constexpr const char *TERRAIN3D_REGION_MAP_TYPE_NAMES[TERRAIN3D_TYPE_MAX_VALUE + 1u] = {
	"TYPE_HEIGHT",
	"TYPE_CONTROL",
	"TYPE_COLOR",
	"TYPE_MAX",
};
#define COLOR_BLACK Color( 0.0f, 0.0f, 0.0f, 1.0f )
#define COLOR_ROUGHNESS Color( 1.0f, 1.0f, 1.0f, 0.5f )
#define COLOR_CHECKED Color( 1.f, 1.f, 1.0f, -1.0f )
#define COLOR_NORMAL Color( 0.5f, 0.5f, 1.0f, 1.0f )
static constexpr float TERRAIN3D_CONTROL_AUTO_FLOAT = std::numeric_limits<float>::denorm_min();
#define COLOR_CONTROL Color( TERRAIN3D_CONTROL_AUTO_FLOAT, 0.f, 0.f, 1.0f )
static const Color TERRAIN3D_REGION_MAP_DEFAULT_COLORS[TERRAIN3D_TYPE_MAX_VALUE + 1u] = {
	COLOR_BLACK,
	COLOR_CONTROL,
	COLOR_ROUGHNESS,
	COLOR_NAN,
};

// For consistency between MSVC, gcc, clang
#ifndef FLT_MAX
#define FLT_MAX __FLT_MAX__
#endif

// Terrain3D::_warnings is uint8_t
#define WARN_MISMATCHED_SIZE 0x01
#define WARN_MISMATCHED_FORMAT 0x02
#define WARN_MISMATCHED_MIPMAPS 0x04
#define WARN_ALL 0xFF

// Global Types

#define V2( x ) Vector2( x, x )
#define V2I( x ) Vector2i( x, x )
#define V3( x ) Vector3( x, x, x )
static const Vector2 V2_ZERO{0.f, 0.f};
static const Vector2 V2_MAX{FLT_MAX, FLT_MAX};
static const Vector2i V2I_ZERO{0, 0};
static const Vector2i V2I_MAX{INT32_MAX, INT32_MAX};
static const Vector3 V3_ZERO{0.f, 0.f, 0.f};
static const Vector3 V3_MAX{FLT_MAX, FLT_MAX, FLT_MAX};
static const Vector3 V3_NAN{NAN, NAN, NAN};
static const Vector3 V3_UP{0.f, 1.f, 0.f};
// static const Vector3 V3_DOWN{ 0.f, -1.f, 0.f };

typedef struct t3dvector2iHash_s {
	std::size_t operator()( const Vector2i &v ) const {
		std::size_t h1 = eastl::hash<int>()( v.x );
		std::size_t h2 = eastl::hash<int>()( v.y );
		return h1 ^ ( h2 << 1 );
	}
} t3dvector2iHash_t;

typedef struct t3dpairVector2iIntHash_s {
	std::size_t operator()( const eastl::pair<Vector2i, int> &p ) const {
		std::size_t h1 = t3dvector2iHash_t{}( p.first );
		std::size_t h2 = eastl::hash<int>{}( p.second );
		return h1 ^ ( h2 << 1 );
	}
} t3dpairVector2iIntHash_t;

typedef struct t3dvector3Hash_s {
	std::size_t operator()( const Vector3 &v ) const {
		std::size_t h1 = eastl::hash<float>()( v.x );
		std::size_t h2 = eastl::hash<float>()( v.y );
		std::size_t h3 = eastl::hash<float>()( v.z );
		return h1 ^ ( h2 << 1 ) ^ ( h3 << 2 );
	}
} t3dvector3Hash_t;

// Set class name for logger.h

#define CLASS_NAME() \
	const String __class__ = get_class_static() + String( "#" ) + String::num_uint64( get_instance_id() ).right( 4 );

#define CLASS_NAME_STATIC( pszName ) static inline const char *__class__ = pszName;

// Validation macros

#define ASSERT( cond, ret )                                                                            \
	if ( !( cond ) ) {                                                                                   \
		UtilityFunctions::push_error( "Assertion '", #cond, "' failed at ", __FILE__, ":", __LINE__ ); \
		return ret;                                                                                  \
	}

#define VOID // a return value for void, to avoid compiler warnings

#define IS_INIT( ret ) \
	if ( !m_pTerrain ) { \
		return ret;  \
	}

#define IS_INIT_MESG( mesg, ret ) \
	if ( !m_pTerrain ) {            \
		LOG( Terrain3D::ERROR, mesg );       \
		return ret;             \
	}

#define IS_INIT_COND( cond, ret ) \
	if ( !m_pTerrain || cond ) {    \
		return ret;             \
	}

#define IS_INIT_COND_MESG( cond, mesg, ret ) \
	if ( !m_pTerrain || cond ) {               \
		LOG( Terrain3D::ERROR, mesg );                  \
		return ret;                        \
	}

#define IS_INSTANCER_INIT( ret )                     \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) { \
		return ret;                                \
	}

#define IS_INSTANCER_INIT_MESG( mesg, ret )          \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) { \
		LOG( Terrain3D::ERROR, mesg );                          \
		return ret;                                \
	}

#define IS_DATA_INIT( ret )                     \
	if ( !m_pTerrain || !m_pTerrain->getData() ) { \
		return ret;                           \
	}

#define IS_DATA_INIT_MESG( mesg, ret )          \
	if ( !m_pTerrain || !m_pTerrain->getData() ) { \
		LOG( Terrain3D::ERROR, mesg );                     \
		return ret;                           \
	}

#endif // TERRAIN3D_CONSTANTS_H
