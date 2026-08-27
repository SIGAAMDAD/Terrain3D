
layout(location = 0) in ivec2 in_grid;
layout(location = 1) in uvec2 in_meta;

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
} frame_data;

uint t3d_view_index() { return min(uint(gl_ViewIndex), 1u); }

layout(set = 0, binding = 1, std430) readonly buffer RegionMapData { int region_map[]; };
layout(set = 0, binding = 2) uniform sampler2DArray height_maps;
layout(set = 0, binding = 12) uniform sampler2D displacement_buffer;
layout(set = 0, binding = 14) uniform sampler2DArray derived_maps;
layout(set = 0, binding = 16) uniform sampler2DArray middle_height_maps;
layout(set = 0, binding = 17) uniform sampler2D middle_height_region_map;
layout(set = 0, binding = 18) uniform sampler2D far_height_map;
layout(set = 0, binding = 21) uniform sampler2D world_noise_cache;

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
layout(set = 0, binding = 13, std430) readonly buffer MaterialConfigBuffer { MaterialConfig material_config; };

struct PatchData {
    vec2 origin;
    float scale;
    uint lod;
    uint tier;
    uint stitch_x;
    uint stitch_z;
    uint flags;
    float bounds_min_y;
    float bounds_max_y;
    float bounds_half_extent;
    float previous_origin_x;
    float previous_origin_z;
    float previous_scale;
    float padding_0;
    float padding_1;
};
layout(set = 0, binding = 9, std430) readonly buffer PatchBuffer { PatchData patches[]; };
layout(set = 0, binding = 10, std430) readonly buffer VisibleBuffer { uint visible_indices[]; };
layout(push_constant, std430) uniform DrawData {
    uint direct_patch_index;
    uint direct_mode;
    uint padding_0;
    uint padding_1;
} draw_data;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) flat out uint patch_tier;
layout(location = 3) flat out int terrain_region_layer;
layout(location = 4) out vec2 terrain_world_texel;
#ifdef T3D_FORWARD_MOTION
layout(location = 5) out vec4 current_screen_position;
layout(location = 6) out vec4 previous_screen_position;
#endif

const uint T3D_FEATURE_WORLD_NOISE = 1u << 5u;
const uint T3D_FEATURE_WORLD_NOISE_CACHE = 1u << 12u;
const uint T3D_FEATURE_MACRO_NOISE = 1u << 13u;
const uint T3D_FEATURE_DISPLACEMENT = 1u << 6u;
const uint T3D_FEATURE_DERIVED_MAPS = 1u << 7u;
const uint T3D_FEATURE_MIDDLE_HEIGHT = 1u << 9u;
const uint T3D_FEATURE_FAR_ATLAS = 1u << 10u;

int floor_div_i(int value, int divisor) {
    int q = value / divisor;
    int r = value - q * divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}
int floor_mod_i(int value, int divisor) { return value - floor_div_i(value, divisor) * divisor; }

uint packed_region_cell(ivec2 region_cell) {
    int map_size = int(frame_data.terrain_params.y + 0.5);
    ivec2 map_pos = region_cell + ivec2(map_size / 2);
    if (map_pos.x < 0 || map_pos.y < 0 || map_pos.x >= map_size || map_pos.y >= map_size) return 0u;
    return uint(region_map[map_pos.y * map_size + map_pos.x]);
}
int region_layer(ivec2 region_cell) { return int(packed_region_cell(region_cell) & 0xFFFFu) - 1; }
uint region_shadowmask_mode(ivec2 region_cell) { return (packed_region_cell(region_cell) >> 16u) & 0x3u; }
bool logical_region_present(ivec2 region_cell) { return (packed_region_cell(region_cell) & 0x80000000u) != 0u; }
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

float decode_height(float encoded) { return fma(encoded, frame_data.terrain_params.w, frame_data.terrain_params.z); }
bool far_atlas_contains(vec2 world_texel) {
    float half_world_size = frame_data.terrain_params.x * frame_data.terrain_params.y * 0.5;
    return all(greaterThanEqual(world_texel, vec2(-half_world_size))) && all(lessThan(world_texel, vec2(half_world_size)));
}
vec2 far_height_uv(vec2 world_texel) {
    float world_size = max(frame_data.terrain_params.x * frame_data.terrain_params.y, 1.0);
    vec2 atlas_size = vec2(textureSize(far_height_map, 0));
    vec2 half_texel = vec2(0.5) / max(atlas_size, vec2(1.0));
    return clamp((world_texel + vec2(world_size * 0.5)) / world_size + half_texel, half_texel, vec2(1.0) - half_texel);
}
float sample_far_height(vec2 world_texel) {
    if ((material_config.feature_flags & T3D_FEATURE_FAR_ATLAS) == 0u || !far_atlas_contains(world_texel)) return material_config.ground_level;
    return decode_height(textureLod(far_height_map, far_height_uv(world_texel), 0.0).r);
}

float world_noise_hash(vec2 v) {
    return fract(1e4 * sin(fma(17.0, v.x, v.y * 0.1)) * (0.1 + abs(sin(fma(v.y, 13.0, v.x)))));
}
vec3 world_noise_2d(vec2 x) {
    vec2 f = fract(x);
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 s = f - 1.0;
    vec2 u = f3 * fma(vec2(6.0), f2, fma(vec2(-15.0), f, vec2(10.0)));
    vec2 du = 30.0 * f2 * s * s;
    vec2 p = floor(x);
    float a = world_noise_hash(p);
    float b = world_noise_hash(p + vec2(1.0, 0.0));
    float c = world_noise_hash(p + vec2(0.0, 1.0));
    float d = world_noise_hash(p + vec2(1.0, 1.0));
    float k0 = a;
    float k1 = b - a;
    float k2 = c - a;
    float k3 = d - (b + k2);
    return vec3(fma(k2, u.y, fma(u.x, fma(k3, u.y, k1), k0)), du * fma(vec2(k3), u.yx, vec2(k1, k2)));
}
float procedural_world_noise(vec2 world_texel) {
    vec2 p = (world_texel + vec2(material_config.world_noise_offset_x, material_config.world_noise_offset_z) * 1024.0) *
        material_config.world_noise_scale * (0.1 / 1024.0);
    float distance_to_target = length(world_texel * frame_data.mesh_params.x - frame_data.target_params.xy);
    int max_octaves = int(clamp(material_config.world_noise_max_octaves, 0u, 15u));
    int min_octaves = min(int(material_config.world_noise_min_octaves), max_octaves);
    int octaves = int(clamp(float(max_octaves) - floor(distance_to_target / max(material_config.world_noise_lod_distance, 1.0)),
        float(min_octaves), float(max_octaves)));
    bool cache_mode = material_config.world_noise_mode == 1u || material_config.world_noise_mode == 2u ||
        (material_config.world_noise_mode == 0u && material_config.world_noise_cache_enabled != 0u);
    if (cache_mode && (material_config.feature_flags & T3D_FEATURE_WORLD_NOISE_CACHE) != 0u) {
        float cache_lod = clamp(float(max_octaves - octaves), 0.0, material_config.world_noise_cache_max_lod);
        vec2 cache_uv = p / max(material_config.world_noise_cache_period, 0.001);
        return textureLod(world_noise_cache, cache_uv, cache_lod).r * 2.0 - 1.0;
    }
    float value = 0.0;
    float amplitude = 1.0;
    vec2 derivative = vec2(0.0);
    for (int octave = 0; octave < 15; ++octave) {
        if (octave >= octaves) break;
        vec3 n = world_noise_2d(p);
        derivative += n.yz;
        value += amplitude * n.x / (1.0 + dot(derivative, derivative));
        amplitude *= 0.5;
        p = mat2(vec2(0.8, -0.6), vec2(0.6, 0.8)) * p * 2.0;
    }
    return value;
}
float world_noise_height(vec2 world_texel) {
    float weight = region_background_blend(world_texel);
    if (weight <= 0.5) return material_config.ground_level;
    float noise_value = procedural_world_noise(world_texel);
    float noise_height = noise_value * material_config.world_noise_height * 10.0 + material_config.world_noise_offset_y * 100.0;
    return material_config.ground_level + noise_height * smoothstep(0.5, 1.0, weight);
}
bool world_noise_uses_fragment_normals() {
#if T3D_TIER != 0
    return false;
#else
    if (material_config.background_mode != 2u || (material_config.feature_flags & T3D_FEATURE_WORLD_NOISE) == 0u) return false;
    if (material_config.world_noise_mode == 2u || material_config.world_noise_mode == 4u) return true;
    return material_config.world_noise_mode == 0u && material_config.world_noise_fragment_normals != 0u;
#endif
}
float sample_background_height(vec2 world_texel) {
    if (material_config.background_mode == 2u && (material_config.feature_flags & T3D_FEATURE_WORLD_NOISE) != 0u) return world_noise_height(world_texel);
    return material_config.ground_level;
}
float sample_background_normal_height(vec2 world_texel) {
    return world_noise_uses_fragment_normals() ? material_config.ground_level : sample_background_height(world_texel);
}

float sample_primary_height_texel_mode(ivec2 texel, bool normal_sample) {
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 region_cell = ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size));
    int layer = region_layer(region_cell);
    if (layer < 0) return (material_config.feature_flags & T3D_FEATURE_FAR_ATLAS) != 0u && far_atlas_contains(vec2(texel)) ? sample_far_height(vec2(texel)) : (normal_sample ? sample_background_normal_height(vec2(texel)) : sample_background_height(vec2(texel)));
    ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
    ivec3 size3 = textureSize(height_maps, 0);
    local = clamp(local, ivec2(0), size3.xy - ivec2(1));
    return decode_height(texelFetch(height_maps, ivec3(local, layer), 0).r);
}
int middle_height_layer(ivec2 region_cell) {
    if ((material_config.feature_flags & T3D_FEATURE_MIDDLE_HEIGHT) == 0u) return -1;
    int map_size = int(frame_data.terrain_params.y + 0.5);
    ivec2 map_pos = region_cell + ivec2(map_size / 2);
    ivec2 texture_size = textureSize(middle_height_region_map, 0);
    if (map_pos.x < 0 || map_pos.y < 0 || map_pos.x >= min(map_size, texture_size.x) || map_pos.y >= min(map_size, texture_size.y)) return -1;
    return int(round(texelFetch(middle_height_region_map, map_pos, 0).r * 65535.0)) - 1;
}
float sample_middle_height_texel_mode(ivec2 texel, bool normal_sample) {
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 region_cell = ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size));
    int layer = middle_height_layer(region_cell);
    if (layer < 0) return (material_config.feature_flags & T3D_FEATURE_FAR_ATLAS) != 0u && far_atlas_contains(vec2(texel)) ? sample_far_height(vec2(texel)) : (normal_sample ? sample_background_normal_height(vec2(texel)) : sample_background_height(vec2(texel)));
    ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
    ivec3 size3 = textureSize(middle_height_maps, 0);
    if (size3.x == region_size && size3.y == region_size) {
        return decode_height(texelFetch(middle_height_maps, ivec3(clamp(local, ivec2(0), size3.xy - ivec2(1)), layer), 0).r);
    }
    vec2 uv = (vec2(local) + vec2(0.5)) / vec2(region_size);
    return decode_height(textureLod(middle_height_maps, vec3(clamp(uv, vec2(0.0), vec2(1.0)), float(layer)), 0.0).r);
}
float sample_height_texel_mode(ivec2 texel, bool normal_sample) {
#if T3D_TIER == 0
    return sample_primary_height_texel_mode(texel, normal_sample);
#elif T3D_TIER == 1
    return sample_middle_height_texel_mode(texel, normal_sample);
#else
    if ((material_config.feature_flags & T3D_FEATURE_FAR_ATLAS) != 0u && far_atlas_contains(vec2(texel))) return sample_far_height(vec2(texel));
    return normal_sample ? sample_background_normal_height(vec2(texel)) : sample_background_height(vec2(texel));
#endif
}
float sample_height_texel(ivec2 texel) { return sample_height_texel_mode(texel, false); }
float sample_height_nearest(vec2 world_xz) {
    return sample_height_texel_mode(ivec2(round(world_xz / frame_data.mesh_params.x)), false);
}
float sample_normal_height_nearest(vec2 world_xz) {
    return sample_height_texel_mode(ivec2(round(world_xz / frame_data.mesh_params.x)), true);
}
float sample_height_interpolated(vec2 world_xz) {
    vec2 world_texel = world_xz / frame_data.mesh_params.x;
    ivec2 base = ivec2(floor(world_texel));
    vec2 f = fract(world_texel);
    float h00 = sample_height_texel(base);
    float h10 = sample_height_texel(base + ivec2(1, 0));
    float h01 = sample_height_texel(base + ivec2(0, 1));
    float h11 = sample_height_texel(base + ivec2(1, 1));
    return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}

vec3 sample_derived_normal(vec2 world_xz, vec3 fallback_normal) {
#if T3D_TIER == 0
    if ((material_config.feature_flags & T3D_FEATURE_DERIVED_MAPS) == 0u || material_config.tier[0].normal_mode != 3u) return fallback_normal;
    vec2 world_texel = world_xz / frame_data.mesh_params.x;
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 texel = ivec2(floor(world_texel));
    ivec2 region_cell = ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size));
    int layer = region_layer(region_cell);
    if (layer < 0) return fallback_normal;
    ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
    ivec3 size3 = textureSize(derived_maps, 0);
    vec2 uv = (vec2(local) + vec2(0.5)) / vec2(region_size);
    vec2 encoded = textureLod(derived_maps, vec3(uv, float(layer)), 0.0).rg;
    vec2 xz = encoded * 2.0 - 1.0;
    float yy = sqrt(max(1.0 - dot(xz, xz), 0.0));
    return normalize(vec3(xz.x, yy, xz.y));
#else
    return fallback_normal;
#endif
}

float edge_axis_offset(int edge_index, int test_axis) {
    float mesh_size = frame_data.mesh_params.y;
    if (edge_index == 0) return test_axis == 2 ? -(4.0 * mesh_size + 6.0) : 0.0;
    if (test_axis == 0) return 4.0 * mesh_size + 8.0;
    return test_axis == 2 ? -2.0 : 0.0;
}
vec2 edge_offset(uint part_code, PatchData patch_data) {
    int part = int(part_code);
    int sx = clamp(int(patch_data.stitch_x), 0, 2);
    int sz = clamp(int(patch_data.stitch_z), 0, 2);
    if (part == 1) return vec2(edge_axis_offset(0, sx), -2.0 * float(sz - 1));
    if (part == 2) return vec2(edge_axis_offset(1, sx), -2.0 * float(sz - 1));
    if (part == 3) return vec2(0.0, edge_axis_offset(0, sz));
    if (part == 4) return vec2(0.0, edge_axis_offset(1, sz));
    return vec2(0.0);
}
vec3 clipmap_morph(vec2 world_xz, vec2 grid_xz, float scale) {
    float inv_scale = 1.0 / max(scale, 1e-6);
    vec2 target_xz = frame_data.target_params.xy;
    float max_xz = max(abs(world_xz.x - target_xz.x), abs(world_xz.y - target_xz.y));
    float denominator = max(frame_data.mesh_params.y - 4.0, 1.0);
    float morph = smoothstep(0.0, 1.0, (max_xz * inv_scale - frame_data.mesh_params.y - 4.0) / denominator);
    vec2 vertex_fract = fract(grid_xz * 0.5) * 2.0;
    vec2 shift;
    float subdiv = max(frame_data.target_params.z, 1.0);
    if (scale < frame_data.mesh_params.x / subdiv + 1e-6) {
        float alternate = round(fract(round(mod(world_xz.y * inv_scale, 4.0)) * round(mod(world_xz.x * inv_scale, 4.0)) * 0.25));
        shift = mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y), alternate);
    } else {
        shift = vertex_fract * round((fract(world_xz * 0.25 * inv_scale) - 0.5) * 4.0);
    }
    return vec3(shift, morph);
}
vec3 sample_clipmap_displacement(vec2 world_xz, float clipmap_scale) {
    if ((material_config.feature_flags & T3D_FEATURE_DISPLACEMENT) == 0u || frame_data.displacement_params.y < 0.5 || frame_data.target_params.z <= 1.0) return vec3(0.0);
    float vertex_density = 1.0 / max(frame_data.mesh_params.x, 1e-6);
    float lod_scale = floor(log2(1.0 / max(clipmap_scale * vertex_density, 1e-6)));
    float sample_scale = exp2(lod_scale) * 0.5;
    float subdivision_levels = max(log2(frame_data.target_params.z), 1.0);
    vec2 position_texel = world_xz * vertex_density;
    vec2 displacement_uv = (sample_scale * position_texel - round(frame_data.target_params.xy * vertex_density * sample_scale))
        / max(frame_data.mesh_params.y * 2.0, 1.0) + vec2(0.5);
    displacement_uv.x += lod_scale - 1.0;
    displacement_uv.x /= subdivision_levels;
    if (any(lessThan(displacement_uv, vec2(0.0))) || any(greaterThan(displacement_uv, vec2(1.0)))) return vec3(0.0);
    return (textureLod(displacement_buffer, displacement_uv, 0.0).rgb * 2.0 - 1.0) * frame_data.displacement_params.x;
}

void main() {
    uint patch_index = draw_data.direct_mode != 0u ? draw_data.direct_patch_index : visible_indices[gl_InstanceIndex];
    PatchData patch_data = patches[patch_index];
    vec2 grid = vec2(in_grid) + edge_offset(in_meta.x, patch_data);
    vec2 start_world_xz = grid * patch_data.scale + patch_data.origin;
    vec3 morph = clipmap_morph(start_world_xz, grid, patch_data.scale);
    vec2 end_world_xz = start_world_xz - morph.xy * patch_data.scale;
    vec2 world_xz = mix(start_world_xz, end_world_xz, morph.z);
    float height = patch_data.scale < frame_data.mesh_params.x ? sample_height_interpolated(world_xz)
        : mix(sample_height_nearest(start_world_xz), sample_height_nearest(end_world_xz), morph.z);
#if !defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS)
    float normal_step = max(frame_data.mesh_params.x, patch_data.scale);
    float h_l = sample_normal_height_nearest(world_xz - vec2(normal_step, 0.0));
    float h_r = sample_normal_height_nearest(world_xz + vec2(normal_step, 0.0));
    float h_d = sample_normal_height_nearest(world_xz - vec2(0.0, normal_step));
    float h_u = sample_normal_height_nearest(world_xz + vec2(0.0, normal_step));
    vec3 geometry_normal = normalize(vec3(h_l - h_r, 2.0 * normal_step, h_d - h_u));
    world_normal = sample_derived_normal(world_xz, geometry_normal);
#else
    world_normal = vec3(0.0, 1.0, 0.0);
#endif
    vec3 displacement_start = sample_clipmap_displacement(start_world_xz, patch_data.scale);
    vec3 displacement_end = sample_clipmap_displacement(end_world_xz, patch_data.scale * 2.0);
    vec3 displacement = mix(displacement_start, displacement_end, morph.z);
    world_position = vec3(world_xz.x, height, world_xz.y) + displacement;
    terrain_world_texel = world_xz / frame_data.mesh_params.x;
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 texel = ivec2(floor(terrain_world_texel));
    terrain_region_layer = region_layer(ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size)));
    patch_tier = patch_data.tier;
    vec4 current_clip = frame_data.view_projection[t3d_view_index()] * vec4(world_position, 1.0);
    gl_Position = current_clip;
#ifdef T3D_FORWARD_MOTION
    current_screen_position = current_clip;
    previous_screen_position = frame_data.motion_params.x > 0.5 ?
        frame_data.previous_view_projection[t3d_view_index()] * vec4(world_position, 1.0) : current_clip;
#endif
}


