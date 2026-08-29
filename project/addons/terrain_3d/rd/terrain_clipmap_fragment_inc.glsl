#define T3D_DIV_255 0.0039215686274509803921568627451
#define T3D_TAU_16 0.39269908169872415480783042290994

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) flat in uint patch_tier;
layout(location = 3) flat in int terrain_region_layer;
layout(location = 4) in vec2 terrain_world_texel;
#ifdef T3D_FORWARD_MOTION
layout(location = 5) in vec4 current_screen_position;
layout(location = 6) in vec4 previous_screen_position;
#endif
#ifdef T3D_FORWARD_DEPTH
#ifdef T3D_FORWARD_NORMAL_ROUGHNESS
layout(location = 0) out vec4 out_normal_roughness;
#ifdef T3D_FORWARD_VOXEL_GI
layout(location = 1) out uvec2 out_voxel_gi;
#endif
#endif
#else
layout(location = 0) out vec4 out_color;
#ifdef T3D_FORWARD_SPECULAR
layout(location = 1) out vec4 out_specular;
#endif
#ifdef T3D_FORWARD_MOTION
layout(location = 2) out vec2 out_motion;
#endif
#endif

layout(set = 0, binding = 0, std140) uniform FrameData {
    mat4 view_projection[2];
    mat4 view_matrix;
    mat4 previous_view_projection[2];
    vec4 motion_jitter;
    vec4 motion_params;
    vec4 light_direction_ambient;
    vec4 light_color;
    vec4 terrain_params;
    vec4 mesh_params;
    vec4 target_params;
    vec4 camera_params[2];
    vec4 fog_params;
    vec4 fog_depth_params;
    vec4 displacement_params;
}
frame_data;

uint t3d_view_index() {
    return min(uint(gl_ViewIndex), 1u);
}
layout(set = 0, binding = 1, std430) readonly buffer RegionMapData {
    int region_map[];
};
layout(set = 0, binding = 2) uniform sampler2DArray height_maps;
layout(set = 0, binding = 3) uniform sampler2DArray control_maps;
layout(set = 0, binding = 4) uniform sampler2DArray color_maps;
layout(set = 0, binding = 5) uniform sampler2DArray albedo_maps;
layout(set = 0, binding = 6) uniform sampler2DArray normal_maps;
layout(set = 0, binding = 7) uniform sampler2DArray surface_maps;
layout(set = 0, binding = 12) uniform sampler2D displacement_buffer;
layout(set = 0, binding = 14) uniform sampler2DArray derived_maps;
layout(set = 0, binding = 15) uniform sampler2DArray middle_composite_maps;
layout(set = 0, binding = 16) uniform sampler2DArray middle_height_maps;
layout(set = 0, binding = 17) uniform sampler2D middle_height_region_map;
layout(set = 0, binding = 18) uniform sampler2D far_height_map;
layout(set = 0, binding = 19) uniform sampler2D far_color_map;
layout(set = 0, binding = 20) uniform sampler2DArray far_albedo_maps;
layout(set = 0, binding = 21) uniform sampler2D world_noise_cache;
layout(set = 0, binding = 22) uniform sampler2D macro_noise_texture;

struct EditorDecal {
    vec4 transform; // world position.xy, size, rotation
    vec4 color;
    uvec4 state; // visible, part flags, padding
};
struct EditorOverlayConfig {
    EditorDecal decals[3];
    vec4 heightmap_contour; // black height, white height, interval, thickness
    vec4 contour_color;
    vec4 instancer_grid; // cell size, padding
};
layout(set = 0, binding = 23, std430) readonly buffer EditorOverlayBuffer {
    EditorOverlayConfig editor_overlay;
};
layout(set = 0, binding = 24) uniform sampler2D editor_brush_texture;
layout(set = 0, binding = 25) uniform sampler2DArray terrain_shadow_masks;

struct MaterialLayer {
    vec4 color;
    vec4 pbr0;
    vec4 pbr1;
    vec2 displacement;
    uint physical_layer;
    uint flags;
};
layout(set = 0, binding = 8, std430) readonly buffer MaterialBuffer {
    MaterialLayer materials[];
};
layout(set = 0, binding = 11) uniform sampler2DArray displacement_maps;

struct TierMaterialConfig {
    uint sampling_mode;
    uint normal_mode;
    uint color_mode;
    uint surface_response;
    uint texture_surface_mode;
    uint lighting_model;
    uint detiling_mode;
    uint filtering;
    float authored_color_contribution;
    float wetness_contribution;
    float material_lod_bias;
    float quality_lod_offset;
};
struct MaterialConfig {
    uint feature_flags;
    uint debug_flags;
    uint output_flags;
    uint background_mode;
    uint projection_mode;
    uint tessellation_material_mode;
    uint world_noise_mode;
    uint texture_layer_counts;
    float displacement_sharpness;
    float blend_sharpness;
    float distant_normal_scale;
    float ground_level;
    float region_blend;
    float bias_distance;
    float mipmap_bias;
    float depth_blur;
    float auto_slope;
    float auto_height_reduction;
    uint auto_base_texture;
    uint auto_overlay_texture;
    uint dual_scale_texture;
    uint far_albedo_layer_count;
    float dual_scale_reduction;
    float tri_scale_reduction;
    float dual_scale_far;
    float dual_scale_near;
    float noise1_scale;
    float noise2_scale;
    float noise1_rotation_x;
    float noise1_rotation_y;
    float macro_variation1_r;
    float macro_variation1_g;
    float macro_variation1_b;
    float macro_variation_slope;
    float macro_variation2_r;
    float macro_variation2_g;
    float macro_variation2_b;
    float macro_variation_padding;
    float noise1_offset_x;
    float noise1_offset_y;
    float world_noise_cache_period;
    float world_noise_lod_distance;
    float world_noise_scale;
    float world_noise_height;
    uint world_noise_max_octaves;
    uint world_noise_min_octaves;
    uint world_noise_cache_enabled;
    uint world_noise_fragment_normals;
    float world_noise_offset_x;
    float world_noise_offset_y;
    float world_noise_offset_z;
    float world_noise_cache_max_lod;
    float silhouette_r;
    float silhouette_g;
    float silhouette_b;
    float silhouette_a;
    float contour_step;
    float editor_overlay_opacity;
    float reserved_2;
    float reserved_3;
    TierMaterialConfig tier[3];
};
layout(set = 0, binding = 13, std430) readonly buffer MaterialConfigBuffer {
    MaterialConfig material_config;
};

const uint T3D_FEATURE_AUTO_SHADER = 1u << 0u;
const uint T3D_FEATURE_DUAL_SCALING = 1u << 1u;
const uint T3D_FEATURE_MACRO_VARIATION = 1u << 2u;
const uint T3D_FEATURE_CONTROL_IMPLICIT = 1u << 3u;
const uint T3D_FEATURE_COLOR_IMPLICIT = 1u << 4u;
const uint T3D_FEATURE_WORLD_NOISE = 1u << 5u;
const uint T3D_FEATURE_WORLD_NOISE_CACHE = 1u << 12u;
const uint T3D_FEATURE_MACRO_NOISE = 1u << 13u;
const uint T3D_FEATURE_DISPLACEMENT = 1u << 6u;
const uint T3D_FEATURE_DERIVED_MAPS = 1u << 7u;
const uint T3D_FEATURE_MIDDLE_COMPOSITE = 1u << 8u;
const uint T3D_FEATURE_MIDDLE_HEIGHT = 1u << 9u;
const uint T3D_FEATURE_FAR_ATLAS = 1u << 10u;
const uint T3D_FEATURE_FAR_ALBEDO = 1u << 11u;
const uint T3D_LAYER_PACKED_SURFACE = 1u << 0u;
const uint T3D_LAYER_RESIDENT = 1u << 1u;
const uint T3D_OUTPUT_ALBEDO = 1u << 0u;
const uint T3D_OUTPUT_ROUGHNESS = 1u << 1u;
const uint T3D_OUTPUT_SPECULAR = 1u << 2u;
const uint T3D_OUTPUT_NORMAL = 1u << 3u;
const uint T3D_OUTPUT_AO = 1u << 4u;

const uint T3D_DEBUG_REGION_GRID = 1u << 0u;
const uint T3D_DEBUG_INSTANCER_GRID = 1u << 1u;
const uint T3D_DEBUG_VERTEX_GRID = 1u << 2u;
const uint T3D_DEBUG_CONTOURS = 1u << 3u;
const uint T3D_DEBUG_NAVIGATION = 1u << 4u;
const uint T3D_DEBUG_CHECKERED = 1u << 5u;
const uint T3D_DEBUG_GREY = 1u << 6u;
const uint T3D_DEBUG_HEIGHTMAP = 1u << 7u;
const uint T3D_DEBUG_JAGGEDNESS = 1u << 8u;
const uint T3D_DEBUG_AUTOSHADER = 1u << 9u;
const uint T3D_DEBUG_CONTROL_TEXTURE = 1u << 10u;
const uint T3D_DEBUG_CONTROL_BLEND = 1u << 11u;
const uint T3D_DEBUG_CONTROL_ANGLE = 1u << 12u;
const uint T3D_DEBUG_CONTROL_SCALE = 1u << 13u;
const uint T3D_DEBUG_COLORMAP = 1u << 14u;
const uint T3D_DEBUG_ROUGHMAP = 1u << 15u;
const uint T3D_DEBUG_DISPLACEMENT = 1u << 16u;
const uint T3D_DEBUG_TEX_ALBEDO = 1u << 17u;
const uint T3D_DEBUG_TEX_HEIGHT = 1u << 18u;
const uint T3D_DEBUG_TEX_NORMAL = 1u << 19u;
const uint T3D_DEBUG_TEX_ROUGH = 1u << 20u;
const uint T3D_DEBUG_TEX_AO = 1u << 21u;

int floor_div_i(int value, int divisor) {
    int q = value / divisor;
    int r = value - q * divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}
int floor_mod_i(int value, int divisor) {
    return value - floor_div_i(value, divisor) * divisor;
}
uint packed_region_cell(ivec2 region_cell) {
    int map_size = int(frame_data.terrain_params.y + 0.5);
    ivec2 map_pos = region_cell + ivec2(map_size / 2);
    if (map_pos.x < 0 || map_pos.y < 0 || map_pos.x >= map_size || map_pos.y >= map_size) {
        return 0u;
    }
    return uint(region_map[map_pos.y * map_size + map_pos.x]);
}
int region_layer(ivec2 region_cell) {
    return int(packed_region_cell(region_cell) & 0xFFFFu) - 1;
}
uint region_shadowmask_mode(ivec2 region_cell) {
    return (packed_region_cell(region_cell) >> 16u) & 0x3u;
}
bool logical_region_present(ivec2 region_cell) {
    return (packed_region_cell(region_cell) & 0x80000000u) != 0u;
}
float region_presence(ivec2 base_cell, vec2 weights) {
    float a = logical_region_present(base_cell) ? 1.0 : 0.0;
    float b = logical_region_present(base_cell + ivec2(1, 0)) ? 1.0 : 0.0;
    float c = logical_region_present(base_cell + ivec2(0, 1)) ? 1.0 : 0.0;
    float d = logical_region_present(base_cell + ivec2(1, 1)) ? 1.0 : 0.0;
    return mix(mix(a, b, weights.x), mix(c, d, weights.x), weights.y);
}
float region_background_blend(vec2 world_texel) {
    float region_size = max(frame_data.terrain_params.x, 1.0);
    vec2 uv2 = world_texel / region_size - vec2(0.5011);
    ivec2 base_cell = ivec2(floor(uv2));
    vec2 f = fract(uv2);
    float blend_factor = 2.0 + 126.0 * (1.0 - clamp(material_config.region_blend, 0.0, 1.0));
    float half_width = clamp(0.75 / blend_factor, 1.0 / 1024.0, 0.499);
    vec2 weights = smoothstep(vec2(0.5 - half_width), vec2(0.5 + half_width), f);
    return clamp((1.0 - region_presence(base_cell, weights)) * 2.0, 0.0, 1.0);
}

#if T3D_TIER == 0
float fragment_world_noise_hash(vec2 v) {
    return fract(1e4 * sin(fma(17.0, v.x, v.y * 0.1)) * (0.1 + abs(sin(fma(v.y, 13.0, v.x)))));
}
vec3 fragment_world_noise_2d(vec2 x) {
    vec2 f = fract(x);
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 s = f - 1.0;
    vec2 u = f3 * fma(vec2(6.0), f2, fma(vec2(-15.0), f, vec2(10.0)));
    vec2 du = 30.0 * f2 * s * s;
    vec2 p = floor(x);
    float a = fragment_world_noise_hash(p);
    float b = fragment_world_noise_hash(p + vec2(1.0, 0.0));
    float c = fragment_world_noise_hash(p + vec2(0.0, 1.0));
    float d = fragment_world_noise_hash(p + vec2(1.0, 1.0));
    float k1 = b - a;
    float k2 = c - a;
    float k3 = d - (b + k2);
    return vec3(fma(k2, u.y, fma(u.x, fma(k3, u.y, k1), a)), du * fma(vec2(k3), u.yx, vec2(k1, k2)));
}
vec3 fragment_world_noise_sample(vec2 world_texel) {
    float base_scale = material_config.world_noise_scale * (0.1 / 1024.0);
    vec2 p = (world_texel + vec2(material_config.world_noise_offset_x, material_config.world_noise_offset_z) * 1024.0) * base_scale;
    float distance_to_target = length(world_texel * frame_data.mesh_params.x - frame_data.target_params.xy);
    int max_octaves = int(clamp(material_config.world_noise_max_octaves, 0u, 15u));
    int min_octaves = min(int(material_config.world_noise_min_octaves), max_octaves);
    int octaves = int(clamp(float(max_octaves) - floor(distance_to_target / max(material_config.world_noise_lod_distance, 1.0)),
        float(min_octaves),
        float(max_octaves)));
    bool cache_mode = material_config.world_noise_mode == 1u || material_config.world_noise_mode == 2u
        || (material_config.world_noise_mode == 0u && material_config.world_noise_cache_enabled != 0u);
    if (cache_mode && (material_config.feature_flags & T3D_FEATURE_WORLD_NOISE_CACHE) != 0u) {
        float cache_lod = clamp(float(max_octaves - octaves), 0.0, material_config.world_noise_cache_max_lod);
        float cache_period = max(material_config.world_noise_cache_period, 0.001);
        vec2 cache_uv = p / cache_period;
        vec2 cache_step = vec2(base_scale / cache_period);
        float center = textureLod(world_noise_cache, cache_uv, cache_lod).r * 2.0 - 1.0;
        float sample_x = textureLod(world_noise_cache, cache_uv + vec2(cache_step.x, 0.0), cache_lod).r * 2.0 - 1.0;
        float sample_z = textureLod(world_noise_cache, cache_uv + vec2(0.0, cache_step.y), cache_lod).r * 2.0 - 1.0;
        return vec3(center, sample_x - center, sample_z - center);
    }
    float value = 0.0;
    float amplitude = 1.0;
    vec2 erosion_derivative = vec2(0.0);
    vec2 value_gradient = vec2(0.0);
    mat2 octave_basis = mat2(1.0);
    const mat2 octave_rotation = mat2(vec2(0.8, -0.6), vec2(0.6, 0.8));
    for (int octave = 0; octave < 15; ++octave) {
        if (octave >= octaves) {
            break;
        }
        vec3 n = fragment_world_noise_2d(p);
        erosion_derivative += n.yz;
        float attenuation = 1.0 / (1.0 + dot(erosion_derivative, erosion_derivative));
        value += amplitude * n.x * attenuation;
        value_gradient += amplitude * (transpose(octave_basis) * n.yz) * attenuation;
        amplitude *= 0.5;
        p = octave_rotation * p * 2.0;
        octave_basis = octave_rotation * octave_basis * 2.0;
    }
    return vec3(value, value_gradient * base_scale);
}
vec3 fragment_world_noise_height_sample(vec2 world_texel, float weight) {
    if (weight <= 0.5) {
        return vec3(material_config.ground_level, 0.0, 0.0);
    }
    vec3 noise = fragment_world_noise_sample(world_texel);
    float height_scale = material_config.world_noise_height * 10.0;
    float blend = smoothstep(0.5, 1.0, weight);
    float noise_height = noise.x * height_scale + material_config.world_noise_offset_y * 100.0;
    return vec3(material_config.ground_level + noise_height * blend, noise.yz * height_scale * blend);
}

bool fragment_world_noise_normals_enabled() {
    if (material_config.background_mode != 2u || (material_config.feature_flags & T3D_FEATURE_WORLD_NOISE) == 0u) {
        return false;
    }
    if (material_config.world_noise_mode == 2u || material_config.world_noise_mode == 4u) {
        return true;
    }
    return material_config.world_noise_mode == 0u && material_config.world_noise_fragment_normals != 0u;
}
vec3 apply_fragment_world_noise_normal(vec3 base_normal, vec2 world_texel) {
    if (!fragment_world_noise_normals_enabled()) {
        return base_normal;
    }
    float half_world_size = frame_data.terrain_params.x * frame_data.terrain_params.y * 0.5;
    bool inside_far_atlas = all(greaterThanEqual(world_texel, vec2(-half_world_size))) && all(lessThan(world_texel, vec2(half_world_size)));
    if ((material_config.feature_flags & T3D_FEATURE_FAR_ATLAS) != 0u && inside_far_atlas) {
        return base_normal;
    }
    float weight = region_background_blend(world_texel);
    if (weight <= 0.5) {
        return base_normal;
    }
    vec3 height_sample = fragment_world_noise_height_sample(world_texel, weight);
    float world_step = max(frame_data.mesh_params.x, 0.0001);
    float base_y = max(base_normal.y, 0.0001);
    vec2 base_delta = base_normal.xz * (world_step / base_y);
    return normalize(vec3(base_delta.x - height_sample.y, world_step, base_delta.y - height_sample.z));
}
#else
vec3 apply_fragment_world_noise_normal(vec3 base_normal, vec2 world_texel) {
    return base_normal;
}
#endif
bool far_atlas_contains(vec2 world_texel) {
    float half_world_size = frame_data.terrain_params.x * frame_data.terrain_params.y * 0.5;
    return all(greaterThanEqual(world_texel, vec2(-half_world_size))) && all(lessThan(world_texel, vec2(half_world_size)));
}
vec2 far_atlas_uv(vec2 world_texel) {
    float world_size = max(frame_data.terrain_params.x * frame_data.terrain_params.y, 1.0);
    return clamp((world_texel + vec2(world_size * 0.5)) / world_size, vec2(0.0), vec2(1.0));
}
vec4 sample_far_surface(vec2 world_texel) {
    if ((material_config.feature_flags & T3D_FEATURE_FAR_ATLAS) == 0u || !far_atlas_contains(world_texel)) {
        return vec4(1.0, 1.0, 1.0, 0.0);
    }
    ivec2 size2 = textureSize(far_color_map, 0);
    ivec2 coord = clamp(ivec2(floor(far_atlas_uv(world_texel) * vec2(size2))), ivec2(0), size2 - ivec2(1));
    return texelFetch(far_color_map, coord, 0);
}
uint decode_far_surface_bits(float packed_alpha) {
    return uint(round(clamp(packed_alpha, 0.0, 1.0) * 255.0));
}
bool far_surface_solid(uint bits) {
    return (bits & 0x80u) != 0u;
}
int far_surface_material(uint bits) {
    return int((bits >> 2u) & 0x1Fu);
}
float far_surface_alpha(uint bits) {
    return float(bits & 0x3u) * (1.0 / 3.0);
}

uint read_control(ivec2 texel) {
    if ((material_config.feature_flags & T3D_FEATURE_CONTROL_IMPLICIT) != 0u) {
        return 0u;
    }
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 region_cell = ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size));
    int layer = region_layer(region_cell);
    if (layer < 0) {
        return 0u;
    }
    ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
    ivec3 size3 = textureSize(control_maps, 0);
    local = clamp(local, ivec2(0), size3.xy - ivec2(1));
    return floatBitsToUint(texelFetch(control_maps, ivec3(local, layer), 0).r);
}
vec4 color_texel(ivec2 texel) {
    if ((material_config.feature_flags & T3D_FEATURE_COLOR_IMPLICIT) != 0u) {
        return vec4(1.0);
    }
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 region_cell = ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size));
    int layer = region_layer(region_cell);
    if (layer < 0) {
        return sample_far_surface(vec2(texel));
    }
    ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
    ivec3 size3 = textureSize(color_maps, 0);
    local = clamp(local, ivec2(0), size3.xy - ivec2(1));
    return texelFetch(color_maps, ivec3(local, layer), 0);
}
vec4 sample_color_exact(vec2 world_texel) {
    ivec2 base = ivec2(floor(world_texel));
    vec2 f = fract(world_texel);
    vec4 c00 = color_texel(base);
    vec4 c10 = color_texel(base + ivec2(1, 0));
    vec4 c01 = color_texel(base + ivec2(0, 1));
    vec4 c11 = color_texel(base + ivec2(1, 1));
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}
vec4 sample_color_map(vec2 world_texel, TierMaterialConfig tier_config) {
    if ((material_config.feature_flags & T3D_FEATURE_COLOR_IMPLICIT) != 0u) {
        return vec4(1.0);
    }
    if (terrain_region_layer < 0) {
        return sample_far_surface(world_texel);
    }
    if (tier_config.color_mode == 2u) {
        return sample_color_exact(world_texel);
    }
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 texel = ivec2(floor(world_texel));
    ivec2 region_cell = ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size));
    int layer = region_layer(region_cell);
    if (layer < 0) {
        return sample_far_surface(world_texel);
    }
    ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
    ivec3 size3 = textureSize(color_maps, 0);
    vec2 uv = (vec2(local) + fract(world_texel) + vec2(0.5)) / vec2(max(size3.xy, ivec2(1)));
    return textureLod(color_maps, vec3(uv, float(layer)), 0.0);
}
vec4 sample_middle_composite(vec2 world_texel) {
    if ((material_config.feature_flags & T3D_FEATURE_MIDDLE_COMPOSITE) == 0u || terrain_region_layer < 0) {
        return vec4(0.0);
    }
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 texel = ivec2(floor(world_texel));
    ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
    ivec3 size3 = textureSize(middle_composite_maps, 0);
    vec2 uv = (vec2(local) + fract(world_texel) + vec2(0.5)) / vec2(region_size);
    return textureLod(middle_composite_maps, vec3(clamp(uv, vec2(0.0), vec2(1.0)), float(terrain_region_layer)), 0.0);
}

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float value_noise(vec2 p) {
    ivec2 i = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(vec2(i));
    float b = hash21(vec2(i + ivec2(1, 0)));
    float c = hash21(vec2(i + ivec2(0, 1)));
    float d = hash21(vec2(i + ivec2(1, 1)));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec2 rotate_uv(vec2 uv, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c) * uv;
}
vec2 project_material_uv(
    vec3 position, vec3 normal, float scale, uint projection_mode, out vec2 ddx_uv, out vec2 ddy_uv, out mat2 normal_alignment) {
    vec3 scaled_position = position * scale;
    vec3 scaled_ddx = dFdx(position) * scale;
    vec3 scaled_ddy = dFdy(position) * scale;
    normal_alignment = mat2(1.0);
    if (projection_mode == 0u || normal.y > 0.7071067811865475) {
        ddx_uv = scaled_ddx.xz;
        ddy_uv = scaled_ddy.xz;
        return scaled_position.xz;
    }
    normal_alignment = mat2(vec2(normal.z, -normal.x), vec2(normal.x, normal.z));
    vec2 snapped = round(normalize(-normal.xz) * 1.3065629648763765);
    snapped *= abs(snapped.x) + abs(snapped.y) > 1.5 ? 0.7071067811865475 : 1.0;
    snapped = vec2(-snapped.y, snapped.x);
    ddx_uv = vec2(dot(scaled_ddx.xz, snapped), -scaled_ddx.y);
    ddy_uv = vec2(dot(scaled_ddy.xz, snapped), -scaled_ddy.y);
    return vec2(dot(scaled_position.xz, snapped), -scaled_position.y);
}
vec2 apply_detiling(vec2 uv, vec2 world_xz, uint mode, MaterialLayer layer, out float rotation_angle) {
    rotation_angle = 0.0;
    if (mode == 2u) {
        return uv;
    }
    vec2 cell = floor(world_xz * max(layer.pbr0.x, 0.000001) * 0.25);
    float selector = hash21(cell + vec2(float(layer.physical_layer), 19.0));
    float shift = layer.pbr1.w;
    if (mode == 3u) {
        return uv + vec2(selector - 0.5) * shift;
    }
    uint sectors = mode == 4u ? 4u : (mode == 5u ? 16u : 8u);
    rotation_angle = floor(selector * float(sectors)) * 6.28318530718 / float(sectors) * layer.pbr1.z;
    return rotate_uv(uv, rotation_angle) + vec2(fract(selector * 7.13) - 0.5, fract(selector * 13.71) - 0.5) * shift;
}
vec3 decode_surface_normal(vec2 encoded_xy) {
    vec2 normal_xy = fma(encoded_xy, vec2(2.0), vec2(-1.0));
    float len_sq = dot(normal_xy, normal_xy);
    if (len_sq > 1.0) {
        normal_xy *= inversesqrt(len_sq);
        len_sq = 1.0;
    }
    return vec3(normal_xy.x, sqrt(max(1.0 - len_sq, 0.0)), normal_xy.y);
}

struct SampledMaterial {
    vec3 albedo;
    vec3 tangent_normal;
    float roughness;
    float ao;
    float metallic;
    float height;
    float ao_affect;
    float total_weight;
    int logical_id;
};
SampledMaterial material_zero() {
    SampledMaterial r;
    r.albedo = vec3(0.0);
    r.tangent_normal = vec3(0.0, 0.0, 0.0);
    r.roughness = 0.0;
    r.ao = 0.0;
    r.metallic = 0.0;
    r.height = 0.0;
    r.ao_affect = 0.0;
    r.total_weight = 0.0;
    r.logical_id = 0;
    return r;
}
void accumulate_material(inout SampledMaterial dst, SampledMaterial src, float weight) {
    dst.albedo += src.albedo * weight;
    dst.tangent_normal += src.tangent_normal * weight;
    dst.roughness += src.roughness * weight;
    dst.ao += src.ao * weight;
    dst.metallic += src.metallic * weight;
    dst.height += src.height * weight;
    dst.ao_affect += src.ao_affect * weight;
    dst.total_weight += weight;
    if (weight > 0.5) {
        dst.logical_id = src.logical_id;
    }
}
int material_texture_layer_count(uint shift) {
    return max(int((material_config.texture_layer_counts >> shift) & 0xFFu), 1);
}
SampledMaterial sample_material(int logical_id,
    vec2 world_xz,
    vec3 terrain_normal,
    float control_scale,
    float control_angle,
    TierMaterialConfig tier_config,
    bool use_far_array) {
    int count = max(int(frame_data.mesh_params.w + 0.5), 1);
    int id = clamp(logical_id, 0, count - 1);
    MaterialLayer layer = materials[id];
    bool resident = (layer.flags & T3D_LAYER_RESIDENT) != 0u;
    bool packed_surface = (layer.flags & T3D_LAYER_PACKED_SURFACE) != 0u;
    int texture_layers = use_far_array ? max(int(material_config.far_albedo_layer_count), 1) : material_texture_layer_count(0u);
    int physical = clamp(int(layer.physical_layer), 0, texture_layers - 1);
    float uv_scale = max(layer.pbr0.x, 0.000001) * max(control_scale, 0.1);
    vec2 ddx_uv;
    vec2 ddy_uv;
    mat2 normal_alignment;
    vec2 uv = project_material_uv(vec3(world_xz.x, world_position.y, world_xz.y),
        terrain_normal,
        uv_scale,
        material_config.projection_mode,
        ddx_uv,
        ddy_uv,
        normal_alignment);
    uv = rotate_uv(uv, control_angle);
    ddx_uv = rotate_uv(ddx_uv, control_angle);
    ddy_uv = rotate_uv(ddy_uv, control_angle);
    float detile_angle;
    uv = apply_detiling(uv, world_xz, tier_config.detiling_mode, layer, detile_angle);
    float material_angle = control_angle + detile_angle;
    ddx_uv = rotate_uv(ddx_uv, detile_angle);
    ddy_uv = rotate_uv(ddy_uv, detile_angle);
    float lod_bias = tier_config.material_lod_bias + tier_config.quality_lod_offset;
    vec4 albedo_texel = vec4(1.0);
    if (resident) {
        albedo_texel = use_far_array ? textureGrad(far_albedo_maps, vec3(uv, float(physical)), ddx_uv, ddy_uv)
                                     : textureGrad(albedo_maps, vec3(uv, float(physical)), ddx_uv, ddy_uv);
    }
    float dual_far_factor = 0.0;
    vec2 dual_far_uv = uv;
    vec2 dual_far_ddx = ddx_uv;
    vec2 dual_far_ddy = ddy_uv;
    mat2 dual_far_normal_alignment = normal_alignment;
    float dual_far_material_angle = material_angle;
    if ((material_config.feature_flags & T3D_FEATURE_DUAL_SCALING) != 0u && uint(id) == material_config.dual_scale_texture) {
        float distance_to_camera = length(frame_data.camera_params[t3d_view_index()].xyz - world_position);
        dual_far_factor = smoothstep(material_config.dual_scale_near,
            max(material_config.dual_scale_far, material_config.dual_scale_near + 0.001),
            distance_to_camera);
        dual_far_uv = project_material_uv(vec3(world_xz.x, world_position.y, world_xz.y),
            terrain_normal,
            uv_scale * max(material_config.dual_scale_reduction, 0.001),
            material_config.projection_mode,
            dual_far_ddx,
            dual_far_ddy,
            dual_far_normal_alignment);
        dual_far_uv = rotate_uv(dual_far_uv, control_angle);
        dual_far_ddx = rotate_uv(dual_far_ddx, control_angle);
        dual_far_ddy = rotate_uv(dual_far_ddy, control_angle);
        float far_detile_angle;
        dual_far_uv = apply_detiling(
            dual_far_uv, world_xz * max(material_config.dual_scale_reduction, 0.001), tier_config.detiling_mode, layer, far_detile_angle);
        dual_far_ddx = rotate_uv(dual_far_ddx, far_detile_angle);
        dual_far_ddy = rotate_uv(dual_far_ddy, far_detile_angle);
        dual_far_material_angle = control_angle + far_detile_angle;
        vec4 far_texel = vec4(1.0);
        if (resident) {
            far_texel = use_far_array ? textureGrad(far_albedo_maps, vec3(dual_far_uv, float(physical)), dual_far_ddx, dual_far_ddy)
                                      : textureGrad(albedo_maps, vec3(dual_far_uv, float(physical)), dual_far_ddx, dual_far_ddy);
        }
        albedo_texel = mix(albedo_texel, far_texel, dual_far_factor);
    }
    SampledMaterial result = material_zero();
    result.albedo = albedo_texel.rgb * layer.color.rgb;
    result.height = packed_surface && resident && !use_far_array && material_texture_layer_count(24u) > physical
        ? textureGrad(displacement_maps, vec3(uv, float(physical)), ddx_uv, ddy_uv).r
        : (packed_surface ? 0.5 : albedo_texel.a);
    if (dual_far_factor > 0.0 && packed_surface && resident && !use_far_array && material_texture_layer_count(24u) > physical) {
        float far_height = textureGrad(displacement_maps, vec3(dual_far_uv, float(physical)), dual_far_ddx, dual_far_ddy).r;
        result.height = mix(result.height, far_height, dual_far_factor);
    }
    result.tangent_normal = vec3(0.0, 1.0, 0.0);
    result.roughness = clamp(1.0 + layer.pbr0.z, 0.0, 1.0);
    result.ao = 1.0;
    result.ao_affect = layer.pbr1.y;
    result.metallic = clamp(layer.pbr0.w, 0.0, 1.0);
    result.total_weight = 1.0;
    result.logical_id = id;
    if (resident && !use_far_array && tier_config.surface_response != 0u) {
        if (packed_surface && material_texture_layer_count(16u) > physical) {
            vec4 surface = textureGrad(surface_maps, vec3(uv, float(physical)), ddx_uv, ddy_uv);
            if (dual_far_factor > 0.0) {
                vec4 far_surface = textureGrad(surface_maps, vec3(dual_far_uv, float(physical)), dual_far_ddx, dual_far_ddy);
                vec3 near_normal = decode_surface_normal(surface.rg);
                vec3 far_normal = decode_surface_normal(far_surface.rg);
                near_normal.xz = rotate_uv(near_normal.xz, material_angle) * normal_alignment;
                far_normal.xz = rotate_uv(far_normal.xz, dual_far_material_angle) * dual_far_normal_alignment;
                result.tangent_normal = normalize(mix(near_normal, far_normal, dual_far_factor));
                surface.ba = mix(surface.ba, far_surface.ba, dual_far_factor);
            } else {
                result.tangent_normal = decode_surface_normal(surface.rg);
                result.tangent_normal.xz = rotate_uv(result.tangent_normal.xz, material_angle) * normal_alignment;
            }
            result.roughness = clamp(surface.b + layer.pbr0.z, 0.0, 1.0);
            result.ao = mix(1.0, surface.a, clamp(layer.pbr1.x, 0.0, 1.0));
            if ((material_config.feature_flags & T3D_FEATURE_DISPLACEMENT) != 0u) {
                result.ao = mix(result.ao, 1.0, result.height * result.height);
            }
        } else if (material_texture_layer_count(8u) > physical) {
            vec4 legacy = textureGrad(normal_maps, vec3(uv, float(physical)), ddx_uv, ddy_uv);
            result.roughness = clamp(legacy.a + layer.pbr0.z, 0.0, 1.0);
            vec3 decoded = fma(legacy.xzy, vec3(2.0), vec3(-1.0));
            float normal_length = length(decoded);
            float ao_raw = normal_length * 2.0 - 1.0;
            float texture_ao = ao_raw * ao_raw * clamp(layer.pbr1.x, 0.0, 1.0) + 1.0 - clamp(layer.pbr1.x, 0.0, 1.0);
            result.ao = mix(texture_ao, 1.0, result.height * result.height);
            if (normal_length > 0.00001) {
                result.tangent_normal = decoded / normal_length;
                result.tangent_normal.xz = rotate_uv(result.tangent_normal.xz, material_angle) * normal_alignment;
            }
            if (dual_far_factor > 0.0) {
                vec4 far_legacy = textureGrad(normal_maps, vec3(dual_far_uv, float(physical)), dual_far_ddx, dual_far_ddy);
                vec3 far_decoded = fma(far_legacy.xzy, vec3(2.0), vec3(-1.0));
                float far_normal_length = length(far_decoded);
                float far_ao_raw = far_normal_length * 2.0 - 1.0;
                float far_ao = far_ao_raw * far_ao_raw * clamp(layer.pbr1.x, 0.0, 1.0) + 1.0 - clamp(layer.pbr1.x, 0.0, 1.0);
                if (far_normal_length > 0.00001) {
                    far_decoded /= far_normal_length;
                    far_decoded.xz = rotate_uv(far_decoded.xz, dual_far_material_angle) * dual_far_normal_alignment;
                    result.tangent_normal = normalize(mix(result.tangent_normal, far_decoded, dual_far_factor));
                }
                result.roughness = mix(result.roughness, clamp(far_legacy.a + layer.pbr0.z, 0.0, 1.0), dual_far_factor);
                result.ao = mix(result.ao, mix(far_ao, 1.0, result.height * result.height), dual_far_factor);
            }
        }
        vec2 normal_xz = result.tangent_normal.xz * layer.pbr0.y;
        result.tangent_normal = normalize(vec3(normal_xz.x, result.tangent_normal.y, normal_xz.y));
    }
    return result;
}

uint auto_control(uint authored_control, vec3 terrain_normal, float height, bool outside_region) {
    bool auto_enabled = (material_config.feature_flags & T3D_FEATURE_AUTO_SHADER) != 0u;
    bool use_auto = auto_enabled && (outside_region || (authored_control & 1u) != 0u);
    if (!use_auto) {
        return authored_control;
    }
    float auto_blend =
        clamp(fma(material_config.auto_slope * 2.0, terrain_normal.y - 1.0, 1.0) - material_config.auto_height_reduction * 0.01 * height,
            0.0,
            1.0);
    uint base_id = material_config.auto_base_texture & 0x1Fu;
    uint overlay_id = material_config.auto_overlay_texture & 0x1Fu;
    bool swap_pair = base_id > overlay_id;
    uint canonical_base = swap_pair ? overlay_id : base_id;
    uint canonical_overlay = swap_pair ? base_id : overlay_id;
    float canonical_blend = swap_pair ? 1.0 - auto_blend : auto_blend;
    return (canonical_base << 27u) | (canonical_overlay << 22u) | ((uint(fma(canonical_blend, 255.0, 0.5)) & 0xFFu) << 14u);
}
void decode_control(uint control, out int base_id, out int overlay_id, out float blend, out float angle, out float scale) {
    base_id = int((control >> 27u) & 0x1Fu);
    overlay_id = int((control >> 22u) & 0x1Fu);
    blend = float((control >> 14u) & 0xFFu) * T3D_DIV_255;
    angle = -float((control >> 10u) & 0xFu) * T3D_TAU_16;
    uint encoded_scale = (control >> 7u) & 0x7u;
    scale = 0.9 - float(((encoded_scale + 3u) % 8u) + 1u) * 0.1;
}
SampledMaterial sample_resolved_control_pair(uint control, vec2 world_xz, vec3 terrain_normal, TierMaterialConfig tier_config) {
    int base_id;
    int overlay_id;
    float blend;
    float angle;
    float scale;
    decode_control(control, base_id, overlay_id, blend, angle, scale);
    bool use_far_array = T3D_TIER == 2 && (material_config.feature_flags & T3D_FEATURE_FAR_ALBEDO) != 0u;
    SampledMaterial base = sample_material(base_id, world_xz, terrain_normal, scale, angle, tier_config, use_far_array);
    SampledMaterial overlay =
        overlay_id == base_id ? base : sample_material(overlay_id, world_xz, terrain_normal, scale, angle, tier_config, use_far_array);
    float sharpness = fma(60.0, material_config.blend_sharpness, 4.0);
    float base_weight = pow(max((1.0 - blend) + base.height, 0.00000001), sharpness);
    float overlay_weight = pow(max(blend + overlay.height, 0.00000001), sharpness);
    float pair_weight_inv = 1.0 / max(base_weight + overlay_weight, 0.00001);
    float height_blend = overlay_weight * pair_weight_inv;
    SampledMaterial result = material_zero();
    accumulate_material(result, base, 1.0 - height_blend);
    accumulate_material(result, overlay, height_blend);
    return result;
}

vec4 corner_weights(vec2 world_texel) {
    vec2 f = fract(world_texel);
    return vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
}
bool use_bilinear_controls(TierMaterialConfig tier_config) {
    if (tier_config.sampling_mode == 2u) {
        return false;
    }
    if (tier_config.sampling_mode == 1u) {
        return true;
    }
    float footprint = max(length(dFdx(world_position.xz)), length(dFdy(world_position.xz))) / max(frame_data.mesh_params.x, 0.0001);
    bool adaptive = footprint < 16.0;
    if (tier_config.sampling_mode == 4u) {
        adaptive = adaptive && length(frame_data.camera_params[t3d_view_index()].xyz - world_position) < material_config.bias_distance;
    }
    return adaptive;
}
SampledMaterial sample_detailed_material(vec2 world_texel, vec3 terrain_normal, TierMaterialConfig tier_config) {
    ivec2 base_texel = ivec2(floor(world_texel));
    if (!use_bilinear_controls(tier_config)) {
        uint resolved_control = auto_control(read_control(base_texel), terrain_normal, world_position.y, terrain_region_layer < 0);
        return sample_resolved_control_pair(resolved_control, world_position.xz, terrain_normal, tier_config);
    }
    ivec2 offsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
    vec4 weights = corner_weights(world_texel);
    if (tier_config.sampling_mode == 5u) {
        int first = 0;
        int second = 1;
        for (int i = 1; i < 4; ++i) {
            if (weights[i] > weights[first]) {
                second = first;
                first = i;
            } else if (i != first && weights[i] > weights[second]) {
                second = i;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (i != first && i != second) {
                weights[i] = 0.0;
            }
        }
        weights /= max(dot(weights, vec4(1.0)), 0.00001);
    }
    SampledMaterial result = material_zero();
    for (int i = 0; i < 4; ++i) {
        if (weights[i] <= 0.00001) {
            continue;
        }
        uint resolved_control =
            auto_control(read_control(base_texel + offsets[i]), terrain_normal, world_position.y, terrain_region_layer < 0);
        SampledMaterial corner = sample_resolved_control_pair(resolved_control, world_position.xz, terrain_normal, tier_config);
        float inv = 1.0 / max(corner.total_weight, 0.00001);
        corner.albedo *= inv;
        corner.tangent_normal *= inv;
        corner.roughness *= inv;
        corner.ao *= inv;
        corner.metallic *= inv;
        corner.height *= inv;
        corner.ao_affect *= inv;
        corner.total_weight = 1.0;
        accumulate_material(result, corner, weights[i]);
    }
    return result;
}

vec3 apply_macro_variation(vec3 albedo, vec2 world_texel, vec3 terrain_normal) {
    if ((material_config.feature_flags & T3D_FEATURE_MACRO_VARIATION) == 0u) {
        return albedo;
    }
    vec2 noise_uv =
        world_texel * material_config.noise1_scale * 0.1 + vec2(material_config.noise1_offset_x, material_config.noise1_offset_y);
    vec2 rotated = vec2(material_config.noise1_rotation_x * noise_uv.x + material_config.noise1_rotation_y * noise_uv.y,
        material_config.noise1_rotation_x * noise_uv.y - material_config.noise1_rotation_y * noise_uv.x);
    bool use_noise_texture = (material_config.feature_flags & T3D_FEATURE_MACRO_NOISE) != 0u;
    float noise1 = use_noise_texture ? texture(macro_noise_texture, rotated).r : value_noise(rotated);
    float noise2_uv_scale = material_config.noise2_scale * 0.1;
    float noise2 =
        use_noise_texture ? texture(macro_noise_texture, world_texel * noise2_uv_scale).r : value_noise(world_texel * noise2_uv_scale);
    vec3 macro1 = vec3(material_config.macro_variation1_r, material_config.macro_variation1_g, material_config.macro_variation1_b);
    vec3 macro2 = vec3(material_config.macro_variation2_r, material_config.macro_variation2_g, material_config.macro_variation2_b);
    vec3 macro = mix(macro1, vec3(1.0), noise1) * mix(macro2, vec3(1.0), noise2);
    return albedo * mix(vec3(1.0), macro, clamp(terrain_normal.y + material_config.macro_variation_slope, 0.0, 1.0));
}

vec3 apply_surface_normal(vec3 terrain_normal, vec3 tangent_normal) {
    vec3 tangent = normalize(cross(terrain_normal, abs(terrain_normal.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 bitangent = normalize(cross(terrain_normal, tangent));
    return normalize(tangent * tangent_normal.x + terrain_normal * tangent_normal.y + bitangent * tangent_normal.z);
}
float sample_terrain_shadowmask(vec2 world_texel, int layer) {
    if (layer < 0) {
        return 1.0;
    }
    float region_size = max(frame_data.terrain_params.x, 1.0);
    ivec2 region_cell = ivec2(floor(world_texel / region_size));
    vec2 local_texel = world_texel - vec2(region_cell) * region_size;
    vec2 uv = clamp(local_texel / region_size, vec2(0.0), vec2(1.0));
    return texture(terrain_shadow_masks, vec3(uv, float(layer))).r;
}

void light_material(vec3 albedo,
    vec3 normal,
    float roughness,
    float metallic,
    float ao,
    float ao_affect,
    TierMaterialConfig tier_config,
    out vec3 diffuse_ambient,
    out vec3 specular) {
    diffuse_ambient = vec3(0.0);
    specular = vec3(0.0);
    if (tier_config.lighting_model == 0u) {
        diffuse_ambient = albedo;
        return;
    }
#ifdef T3D_FORWARD_NATIVE_LIGHTING
    float specular_scale = (material_config.output_flags & T3D_OUTPUT_SPECULAR) != 0u ? 1.0 - roughness : 0.0;
    ivec2 shadow_region = ivec2(floor(terrain_world_texel / max(frame_data.terrain_params.x, 1.0)));
    uint shadowmask_mode = region_shadowmask_mode(shadow_region);
    float shadowmask_visibility = shadowmask_mode != 0u ? sample_terrain_shadowmask(terrain_world_texel, terrain_region_layer) : 1.0;
    vec3 direct_diffuse;
    vec3 direct_specular;
    t3d_forward_direct_lighting(albedo,
        world_position,
        normalize(normal),
        frame_data.view_matrix,
        frame_data.camera_params[t3d_view_index()].xyz,
        roughness,
        metallic,
        specular_scale,
        t3d_forward_light_frame.scene_params.y,
        tier_config.lighting_model,
        true,
        shadowmask_mode,
        shadowmask_visibility,
        direct_diffuse,
        direct_specular);
    vec3 view_position = (frame_data.view_matrix * vec4(world_position, 1.0)).xyz;
    vec3 view_normal = normalize(mat3(frame_data.view_matrix) * normal);
    vec3 view_direction = normalize(-view_position);
    vec3 ambient;
    vec3 environment_specular;
    t3d_forward_environment_lighting(
        albedo, view_normal, view_direction, roughness, metallic, ao, specular_scale, ambient, environment_specular);
    float direct_ao = mix(1.0, ao, clamp(ao_affect, 0.0, 1.0));
    diffuse_ambient = ambient + direct_diffuse * direct_ao;
    specular = (direct_specular + environment_specular) * direct_ao;
#else
    vec3 ambient = albedo * frame_data.light_direction_ambient.w * ao;
    vec3 light_dir = normalize(-frame_data.light_direction_ambient.xyz);
    float ndotl = max(dot(normal, light_dir), 0.0);
    vec3 diffuse = albedo * frame_data.light_color.rgb * ndotl;
    float direct_ao = mix(1.0, ao, clamp(ao_affect, 0.0, 1.0));
    if (tier_config.lighting_model == 1u) {
        diffuse_ambient = ambient + diffuse * direct_ao;
        return;
    }
    vec3 view_dir = normalize(frame_data.camera_params[t3d_view_index()].xyz - world_position);
    vec3 half_dir = normalize(light_dir + view_dir);
    float gloss = max(1.0 - roughness, 0.0);
    float specular_term = pow(max(dot(normal, half_dir), 0.0), mix(4.0, 128.0, gloss * gloss)) * gloss;
    if ((material_config.output_flags & T3D_OUTPUT_SPECULAR) == 0u) {
        specular_term = 0.0;
    }
    vec3 specular_color = mix(vec3(0.04), albedo, metallic);
    diffuse_ambient = ambient + diffuse * (1.0 - metallic * 0.6) * direct_ao;
    specular = specular_color * specular_term * frame_data.light_color.rgb * direct_ao;
#endif
}
vec3 apply_fog(vec3 lit) {
    float fog_density = max(frame_data.fog_params.w, 0.0);
    if (fog_density <= 0.0 && frame_data.fog_depth_params.w <= 0.5) {
        return lit;
    }
    float distance_to_camera = length(frame_data.camera_params[t3d_view_index()].xyz - world_position);
    float fog = 0.0;
    if (frame_data.fog_depth_params.w > 0.5) {
        float fog_range = max(frame_data.fog_depth_params.y - frame_data.fog_depth_params.x, 1e-4);
        fog = clamp((distance_to_camera - frame_data.fog_depth_params.x) / fog_range, 0.0, 1.0);
        fog = pow(fog, max(frame_data.fog_depth_params.z, 0.01));
    } else {
        fog = 1.0 - exp(-fog_density * distance_to_camera);
    }
    return mix(lit, frame_data.fog_params.rgb, clamp(fog, 0.0, 1.0));
}

float fractal_contour_lines(
    float thickness, float interval, vec3 spatial_coords, vec3 normal, vec3 base_ddx, vec3 base_ddy, vec3 camera_pos) {
    float depth = max(log(max(length(spatial_coords - camera_pos) / interval, 1e-6)) * (1.0 / log(2.0)) - 1.0, 1.0);
    float interval_a = interval * exp2(max(floor(depth) - 1.0, 1.0)) * 0.5;
    float interval_b = interval * exp2(max(floor(depth), 1.0)) * 0.5;
    float interval_c = interval * exp2(max(floor(depth + 0.5) - 1.0, 1.0)) * 0.5;
    float y_fwidth = max(abs(base_ddx.y) + abs(base_ddy.y), 1e-6);
    thickness *= smoothstep(0.0, 0.0125, clamp(1.0 - normal.y, 0.0, 1.0));
    float mi = max(0.0, thickness - 1.0);
    float ma = max(1.0, thickness);
    float mx = max(0.0, 1.0 - thickness);
    float inv_a = 1.0 / max(interval_a, 1e-6);
    float inv_b = 1.0 / max(interval_b, 1e-6);
    float inv_c = 1.0 / max(interval_c, 1e-6);
    float f_a = abs(fract((spatial_coords.y + interval_a * 0.5) * inv_a) - 0.5);
    float f_b = abs(fract((spatial_coords.y + interval_b * 0.5) * inv_b) - 0.5);
    float f_c = abs(fract((spatial_coords.y + interval_c * 0.5) * inv_c) - 0.5);
    float df_a = max(y_fwidth * inv_a, 1e-6);
    float df_b = max(y_fwidth * inv_b, 1e-6);
    float df_c = max(y_fwidth * inv_c, 1e-6);
    float denom = max(ma - mi, 1e-6);
    float line_a = clamp((f_a - df_a * mi) / (df_a * denom), mx, 1.0);
    float line_b = clamp((f_b - df_b * mi) / (df_b * denom), mx, 1.0);
    float line_c = clamp((f_c - df_c * mi) / (df_c * denom), mx, 1.0);
    float phase = fract(depth - 0.5);
    return mix(mix(line_a, line_b, fract(depth)), line_c, 4.0 * phase * (1.0 - phase));
}

float editor_reticle(vec2 world_xz, EditorDecal decal) {
    float camera_distance = clamp(length(frame_data.camera_params[t3d_view_index()].xyz - world_position), 0.001, 4000.0);
    float sqrt_distance = sqrt(camera_distance);
    float view_scale = 16.0 / sqrt_distance;
    vec2 cross_uv = (world_xz - decal.transform.xy) * view_scale;
    float brush_radius = max(decal.transform.z * 0.5 * view_scale, 1.0);
    float line_start = brush_radius + log2(camera_distance);
    float line_end = 2.5 * line_start;
    float line_thickness = 0.03 * length(cross_uv) + 0.03 * sqrt_distance;
    vec2 d = abs(cross_uv);
    float horizontal = smoothstep(line_thickness, 0.0, d.y) * step(d.x, line_end) * step(line_start, d.x);
    float vertical = smoothstep(line_thickness, 0.0, d.x) * step(d.y, line_end) * step(line_start, d.y);
    float distance_from_center = length(cross_uv);
    float circle_outer = smoothstep(brush_radius + line_thickness, brush_radius, distance_from_center);
    float circle_inner = 1.0 - smoothstep(brush_radius, brush_radius - line_thickness, distance_from_center);
    return clamp(horizontal + vertical + circle_outer * circle_inner, 0.0, 1.0);
}

vec3 apply_editor_decals(vec3 color) {
    for (int i = 0; i < 3; ++i) {
        EditorDecal decal = editor_overlay.decals[i];
        if (decal.state.x == 0u) {
            continue;
        }
        float size = max(decal.transform.z, 1e-6);
        float c = cos(decal.transform.w);
        float sn = sin(decal.transform.w);
        vec2 local = world_position.xz - decal.transform.xy;
        vec2 rotated = vec2(c * local.x - sn * local.y, sn * local.x + c * local.y);
        vec2 decal_uv = rotated / size;
        float mask = 0.0;
        if (i == 0 && (decal.state.y & 1u) != 0u && abs(decal_uv.x) <= 0.499 && abs(decal_uv.y) <= 0.499) {
            mask = texture(editor_brush_texture, decal_uv + vec2(0.5)).r;
        }
        if ((decal.state.y & 2u) != 0u) {
            mask += editor_reticle(world_position.xz, decal);
        }
        mask = clamp(mask, 0.0, 1.0);
        color = mix(color, decal.color.rgb, mask * mask * decal.color.a);
    }
    return color;
}

vec3 debug_visualization(vec3 color, vec4 color_map, uint control, SampledMaterial material, vec3 terrain_normal, bool used_auto) {
    uint flags = material_config.debug_flags;
    if ((flags & T3D_DEBUG_GREY) != 0u) {
        color = vec3(dot(color, vec3(0.299, 0.587, 0.114)));
    }
    if ((flags & T3D_DEBUG_CHECKERED) != 0u) {
        float checker = mod(floor(terrain_world_texel.x * 0.125) + floor(terrain_world_texel.y * 0.125), 2.0);
        color = mix(vec3(0.12), vec3(0.85), checker);
    }
    if ((flags & T3D_DEBUG_HEIGHTMAP) != 0u) {
        float black_height = editor_overlay.heightmap_contour.x;
        float white_height = editor_overlay.heightmap_contour.y;
        float low_height = min(black_height, white_height);
        float high_height = max(black_height, white_height);
        float normalized_height = clamp((world_position.y - low_height) / max(high_height - low_height, 1e-6), 0.0, 1.0);
        normalized_height = mix(normalized_height, 1.0 - normalized_height, float(white_height < black_height));
        color = vec3(smoothstep(0.0, 1.0, normalized_height));
    }
    if ((flags & T3D_DEBUG_JAGGEDNESS) != 0u) {
        color = vec3(1.0 - terrain_normal.y, terrain_normal.y, 0.0);
    }
    if ((flags & T3D_DEBUG_AUTOSHADER) != 0u) {
        color = used_auto ? vec3(1.0, 0.2, 0.05) : vec3(0.05, 0.3, 1.0);
    }
    if ((flags & T3D_DEBUG_CONTROL_TEXTURE) != 0u) {
        color = vec3(float((control >> 27u) & 0x1Fu) / 31.0, float((control >> 22u) & 0x1Fu) / 31.0, float(control & 0xFFu) / 255.0);
    }
    if ((flags & T3D_DEBUG_CONTROL_BLEND) != 0u) {
        color = vec3(float((control >> 14u) & 0xFFu) * T3D_DIV_255);
    }
    if ((flags & T3D_DEBUG_CONTROL_ANGLE) != 0u) {
        color = vec3(float((control >> 10u) & 0xFu) / 15.0, 0.25, 1.0 - float((control >> 10u) & 0xFu) / 15.0);
    }
    if ((flags & T3D_DEBUG_CONTROL_SCALE) != 0u) {
        color = vec3(float((control >> 7u) & 0x7u) / 7.0);
    }
    if ((flags & T3D_DEBUG_COLORMAP) != 0u) {
        color = color_map.rgb;
    }
    if ((flags & T3D_DEBUG_ROUGHMAP) != 0u || (flags & T3D_DEBUG_TEX_ROUGH) != 0u) {
        color = vec3(material.roughness);
    }
    if ((flags & T3D_DEBUG_TEX_ALBEDO) != 0u) {
        color = material.albedo;
    }
    if ((flags & T3D_DEBUG_TEX_HEIGHT) != 0u) {
        color = vec3(material.height);
    }
    if ((flags & T3D_DEBUG_TEX_NORMAL) != 0u) {
        color = material.tangent_normal * 0.5 + 0.5;
    }
    if ((flags & T3D_DEBUG_TEX_AO) != 0u) {
        color = vec3(material.ao);
    }
    if ((flags & T3D_DEBUG_DISPLACEMENT) != 0u) {
        color = vec3(length(textureLod(displacement_buffer, vec2(0.5), 0.0).rgb * 2.0 - 1.0));
    }
    if ((flags & T3D_DEBUG_REGION_GRID) != 0u) {
        vec2 region_uv = fract(terrain_world_texel / frame_data.terrain_params.x);
        float line = step(min(min(region_uv.x, 1.0 - region_uv.x), min(region_uv.y, 1.0 - region_uv.y)), 0.01);
        color = mix(color, vec3(1.0, 0.2, 0.0), line * material_config.editor_overlay_opacity);
    }
    if ((flags & T3D_DEBUG_VERTEX_GRID) != 0u) {
        vec2 vertex_uv = abs(fract(terrain_world_texel) - 0.5);
        float line = 1.0 - smoothstep(0.45, 0.5, max(vertex_uv.x, vertex_uv.y));
        color = mix(color, vec3(0.2, 1.0, 0.3), line * 0.35 * material_config.editor_overlay_opacity);
    }
    if ((flags & T3D_DEBUG_INSTANCER_GRID) != 0u) {
        float cell_size = max(editor_overlay.instancer_grid.x, 1.0);
        vec2 cell = abs(fract((world_position.xz + cell_size * 0.5) / cell_size) - 0.5) * cell_size;
        float grid_distance = min(cell.x, cell.y);
        float line_thickness = 0.01 * sqrt(max(length(frame_data.camera_params[t3d_view_index()].xyz - world_position), 0.001));
        float line = 1.0 - smoothstep(line_thickness - fwidth(grid_distance), line_thickness + fwidth(grid_distance), grid_distance);
        color = mix(color, vec3(0.05), line * material_config.editor_overlay_opacity);
    }
    if ((flags & T3D_DEBUG_CONTOURS) != 0u) {
        float interval = max(editor_overlay.heightmap_contour.z, 0.25);
        float thickness = max(editor_overlay.heightmap_contour.w, 0.0);
        vec3 base_ddx = dFdx(world_position);
        vec3 base_ddy = dFdy(world_position);
        vec3 geometric_normal = normalize(cross(base_ddy, base_ddx));
        float contour_line = fractal_contour_lines(
            thickness, interval, world_position, geometric_normal, base_ddx, base_ddy, frame_data.camera_params[t3d_view_index()].xyz);
        color = mix(color,
            editor_overlay.contour_color.rgb,
            (1.0 - contour_line) * editor_overlay.contour_color.a * material_config.editor_overlay_opacity);
    }
    if ((flags & T3D_DEBUG_NAVIGATION) != 0u && ((control >> 1u) & 1u) != 0u) {
        color *= mix(vec3(1.0), vec3(0.5, 0.0, 0.85), material_config.editor_overlay_opacity);
    }
    return color;
}

void main() {
    TierMaterialConfig tier_config = material_config.tier[T3D_TIER];
    vec2 world_texel = terrain_world_texel;
#if !defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS)
    vec3 terrain_normal = normalize(world_normal);
    if (tier_config.normal_mode == 2u) {
        terrain_normal = normalize(cross(dFdy(world_position), dFdx(world_position)));
    } else if (tier_config.normal_mode < 2u) {
        terrain_normal = apply_fragment_world_noise_normal(terrain_normal, world_texel);
    }
#endif

    vec4 far_surface = sample_far_surface(world_texel);
    uint far_bits = decode_far_surface_bits(far_surface.a);
    bool outside_region = terrain_region_layer < 0;
    if (outside_region && material_config.background_mode == 0u && !far_surface_solid(far_bits)) {
        discard;
    }

    uint nearest_control = read_control(ivec2(floor(world_texel)));
    if (!outside_region && ((nearest_control >> 2u) & 1u) != 0u) {
        discard;
    }

#if defined(T3D_FORWARD_DEPTH) && !defined(T3D_FORWARD_NORMAL_ROUGHNESS)
#if T3D_TIER == 1
    if (tier_config.sampling_mode == 2u && (material_config.feature_flags & T3D_FEATURE_MIDDLE_COMPOSITE) != 0u
        && terrain_region_layer >= 0) {
        vec4 depth_composite = sample_middle_composite(world_texel);
        uint depth_composite_bits = uint(round(clamp(depth_composite.a, 0.0, 1.0) * 255.0));
        if ((depth_composite_bits & 0x80u) == 0u) {
            discard;
        }
    }
#endif
#else

    bool used_auto = (material_config.feature_flags & T3D_FEATURE_AUTO_SHADER) != 0u && (outside_region || (nearest_control & 1u) != 0u);
    vec4 color_map = sample_color_map(world_texel, tier_config);
    SampledMaterial material = material_zero();

#if T3D_TIER == 2
    if ((material_config.feature_flags & T3D_FEATURE_FAR_ATLAS) != 0u && far_surface_solid(far_bits)) {
        int material_id = far_surface_material(far_bits);
        material = sample_material(material_id,
            world_position.xz,
            terrain_normal,
            1.0,
            0.0,
            tier_config,
            (material_config.feature_flags & T3D_FEATURE_FAR_ALBEDO) != 0u);
        color_map = far_surface;
        color_map.a = far_surface_alpha(far_bits);
    } else {
        uint resolved_control = auto_control(nearest_control, terrain_normal, world_position.y, terrain_region_layer < 0);
        material = sample_resolved_control_pair(resolved_control, world_position.xz, terrain_normal, tier_config);
    }
#elif T3D_TIER == 1
    vec4 composite = sample_middle_composite(world_texel);
    if (tier_config.sampling_mode == 2u && (material_config.feature_flags & T3D_FEATURE_MIDDLE_COMPOSITE) != 0u
        && terrain_region_layer >= 0) {
        uint composite_bits = uint(round(clamp(composite.a, 0.0, 1.0) * 255.0));
        if ((composite_bits & 0x80u) == 0u) {
            discard;
        }
        material = material_zero();
        material.albedo = composite.rgb;
        material.tangent_normal = vec3(0.0, 1.0, 0.0);
        material.roughness = 1.0;
        material.ao = 1.0;
        material.total_weight = 1.0;
        color_map = vec4(1.0, 1.0, 1.0, float(composite_bits & 0x7Fu) * (1.0 / 127.0));
    } else if (outside_region && far_surface_solid(far_bits)) {
        material = sample_material(far_surface_material(far_bits),
            world_position.xz,
            terrain_normal,
            1.0,
            0.0,
            tier_config,
            (material_config.feature_flags & T3D_FEATURE_FAR_ALBEDO) != 0u);
        color_map = far_surface;
        color_map.a = far_surface_alpha(far_bits);
    } else {
        material = sample_detailed_material(world_texel, terrain_normal, tier_config);
    }
#else
    if (outside_region && far_surface_solid(far_bits)) {
        material = sample_material(far_surface_material(far_bits),
            world_position.xz,
            terrain_normal,
            1.0,
            0.0,
            tier_config,
            (material_config.feature_flags & T3D_FEATURE_FAR_ALBEDO) != 0u);
        color_map = far_surface;
        color_map.a = far_surface_alpha(far_bits);
    } else {
        material = sample_detailed_material(world_texel, terrain_normal, tier_config);
    }
#endif

    float inv_weight = 1.0 / max(material.total_weight, 0.00001);
    material.albedo *= inv_weight;
    material.tangent_normal = normalize(material.tangent_normal * inv_weight + vec3(0.0, 0.00001, 0.0));
    material.roughness = clamp(material.roughness * inv_weight, 0.0, 1.0);
    material.ao = clamp(material.ao * inv_weight, 0.0, 1.0);
    material.metallic = clamp(material.metallic * inv_weight, 0.0, 1.0);
    material.height *= inv_weight;
    material.ao_affect *= inv_weight;

    if (tier_config.texture_surface_mode == 1u) {
        material.albedo = vec3(material_config.silhouette_r, material_config.silhouette_g, material_config.silhouette_b);
        material.roughness = 1.0;
        material.metallic = 0.0;
        material.ao = 1.0;
        material.tangent_normal = vec3(0.0, 1.0, 0.0);
    }

    material.albedo = apply_macro_variation(material.albedo, world_texel, terrain_normal);
    material.albedo *= mix(vec3(1.0), color_map.rgb, clamp(tier_config.authored_color_contribution, 0.0, 1.0));
    float wetness = clamp(fma(color_map.a, -2.0, 1.0), 0.0, 1.0) * clamp(tier_config.wetness_contribution, 0.0, 1.0);
    material.roughness = clamp(material.roughness - wetness, 0.0, 1.0);

    vec3 final_normal = terrain_normal;
    if ((material_config.output_flags & T3D_OUTPUT_NORMAL) != 0u && tier_config.surface_response != 0u) {
        final_normal = apply_surface_normal(terrain_normal, material.tangent_normal);
    }
    if ((material_config.output_flags & T3D_OUTPUT_ALBEDO) == 0u) {
        material.albedo = vec3(1.0);
    }
    if ((material_config.output_flags & T3D_OUTPUT_ROUGHNESS) == 0u) {
        material.roughness = 1.0;
    }
    if ((material_config.output_flags & T3D_OUTPUT_AO) == 0u) {
        material.ao = 1.0;
    }

#ifdef T3D_FORWARD_DEPTH
    out_normal_roughness = vec4(normalize(mat3(frame_data.view_matrix) * final_normal) * 0.5 + 0.5, material.roughness * (127.0 / 255.0));
#ifdef T3D_FORWARD_VOXEL_GI
    out_voxel_gi = uvec2(0xffu, 0xffu);
#endif
#else
    vec3 scene_emission = vec3(0.0);
#ifdef T3D_FORWARD_NATIVE_LIGHTING
    vec3 scene_view_position = (frame_data.view_matrix * vec4(world_position, 1.0)).xyz;
    vec3 scene_view_normal = normalize(mat3(frame_data.view_matrix) * final_normal);
    t3d_forward_apply_decals(scene_view_position,
        t3d_forward_light_frame.scene_params.y,
        material.albedo,
        scene_view_normal,
        material.roughness,
        material.metallic,
        material.ao,
        scene_emission);
    scene_emission *= t3d_forward_light_frame.environment_params0.y;
    final_normal = normalize(transpose(mat3(frame_data.view_matrix)) * scene_view_normal);
#endif
    vec3 diffuse_ambient;
    vec3 specular;
    light_material(material.albedo,
        final_normal,
        material.roughness,
        material.metallic,
        material.ao,
        material.ao_affect,
        tier_config,
        diffuse_ambient,
        specular);
#ifdef T3D_FORWARD_NATIVE_LIGHTING
    vec3 reflection_view_position = (frame_data.view_matrix * vec4(world_position, 1.0)).xyz;
    vec3 reflection_view_normal = normalize(mat3(frame_data.view_matrix) * final_normal);
    vec3 reflection_view_direction = normalize(-reflection_view_position);
    specular += t3d_forward_reflection(reflection_view_position,
        reflection_view_normal,
        reflection_view_direction,
        material.roughness,
        material.metallic,
        material.albedo,
        t3d_forward_light_frame.scene_params.y);
#endif
    vec3 lit = diffuse_ambient + scene_emission;
#ifndef T3D_FORWARD_SPECULAR
    lit += specular;
#endif
    lit = debug_visualization(lit, color_map, nearest_control, material, terrain_normal, used_auto);
    lit = apply_editor_decals(lit);
    out_color = vec4(apply_fog(lit), 1.0);
#ifdef T3D_FORWARD_SPECULAR
    out_specular = vec4(specular, material.metallic);
#endif
#ifdef T3D_FORWARD_MOTION
    vec2 current_clip_xy = current_screen_position.xy / current_screen_position.w - frame_data.motion_jitter.xy;
    vec2 previous_clip_xy = previous_screen_position.xy / previous_screen_position.w - frame_data.motion_jitter.zw;
    out_motion = (previous_clip_xy - current_clip_xy) * 0.5;
    if (frame_data.motion_params.y > 0.5) {
        out_color.rgb = clamp(vec3(0.5 + out_motion * 8.0, length(out_motion) * 16.0), vec3(0.0), vec3(1.0));
    }
#endif
#endif // T3D_FORWARD_DEPTH normal/roughness output
#endif // T3D_FORWARD_DEPTH
}
