#[compute]
#version 450

// Raw-GLSL displacement-buffer equivalent for the restricted packed-PBR/R8 path.
// Projection, detiling, custom buffer shaders, and Compatibility rendering remain
// on Terrain3D's existing CanvasItem fallback.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray height_maps;
layout(set = 0, binding = 1) uniform sampler2DArray control_maps;
layout(set = 0, binding = 2) uniform sampler2DArray displacement_maps;
layout(rgba16f, set = 0, binding = 3) restrict writeonly uniform image2D output_image;
layout(set = 0, binding = 4, std430) restrict readonly buffer RegionMap { int region_map[]; };
layout(set = 0, binding = 5, std430) restrict readonly buffer MaterialData { vec4 materials[]; };

layout(push_constant, std430) uniform Params {
    uint width;
    uint height;
    uint region_size;
    uint region_map_size;
    float vertex_spacing;
    float vertex_density;
    float mesh_size;
    float tessellation_level;
    float target_x;
    float target_z;
    float blend_sharpness;
    float displacement_sharpness;
    float auto_slope;
    float auto_height_reduction;
    uint auto_base_texture;
    uint auto_overlay_texture;
    uint auto_enabled;
    uint sampling_mode;
    uint fast_blend_curve;
    uint background_mode;
    float ground_level;
    float region_blend;
    float height_min;
    float height_range;
    uint control_maps_implicit;
    uint texture_layer_count;
    uint padding1;
    uint padding2;
} params;

const float DIV_255 = 1.0 / 255.0;
const float TAU_16TH = -0.392699081698724;

struct MaterialAccumulation {
    float height;
    float total_weight;
};

int region_layer(ivec2 map_pos) {
    if (any(lessThan(map_pos, ivec2(0))) || any(greaterThanEqual(map_pos, ivec2(int(params.region_map_size))))) {
        return -1;
    }
    return region_map[map_pos.y * int(params.region_map_size) + map_pos.x] - 1;
}

ivec3 index_coord(vec2 world_texel) {
    vec2 rounded = round(world_texel);
    ivec2 map_pos = ivec2(floor(rounded / float(params.region_size))) + ivec2(int(params.region_map_size) / 2);
    vec2 wrapped = mod(rounded, float(params.region_size));
    return ivec3(ivec2(wrapped), region_layer(map_pos));
}

void quad_indices(vec2 index_id, out ivec3 i0, out ivec3 i1, out ivec3 i2, out ivec3 i3) {
    i3 = index_coord(index_id);
    int last_texel = int(params.region_size) - 1;
    if (i3.z >= 0 && i3.x < last_texel && i3.y < last_texel) {
        i2 = ivec3(i3.xy + ivec2(1, 0), i3.z);
        i0 = ivec3(i3.xy + ivec2(0, 1), i3.z);
        i1 = ivec3(i3.xy + ivec2(1, 1), i3.z);
    } else {
        i0 = index_coord(index_id + vec2(0.0, 1.0));
        i1 = index_coord(index_id + vec2(1.0, 1.0));
        i2 = index_coord(index_id + vec2(1.0, 0.0));
    }
}

ivec3 offset_index(vec2 index_id, ivec3 base_index, ivec2 offset) {
    ivec2 local = base_index.xy + offset;
    int size_i = int(params.region_size);
    if (base_index.z >= 0 && all(greaterThanEqual(local, ivec2(0))) && all(lessThan(local, ivec2(size_i)))) {
        return ivec3(local, base_index.z);
    }
    return index_coord(index_id + vec2(offset));
}

float check_region(vec2 region_uv) {
    ivec2 map_pos = ivec2(floor(region_uv)) + ivec2(int(params.region_map_size) / 2);
    return float(region_layer(map_pos) >= 0);
}

float flat_region_blend(vec2 region_uv) {
    region_uv -= 0.5011;
    float a = check_region(region_uv + vec2(0.0, 1.0));
    float b = check_region(region_uv + vec2(1.0, 1.0));
    float c = check_region(region_uv + vec2(1.0, 0.0));
    float d = check_region(region_uv);
    vec2 blend_factor = vec2(2.0 + 126.0 * (1.0 - params.region_blend));
    vec2 f = clamp(fract(region_uv), vec2(1e-6), vec2(1.0 - 1e-6));
    vec2 w = 1.0 / (1.0 + exp(blend_factor * log((1.0 - f) / f)));
    float blend = mix(mix(d, c, w.x), mix(a, b, w.x), w.y);
    return (1.0 - blend) * 2.0;
}

float terrain_height(ivec3 index, vec2 world_texel) {
    float height = index.z >= 0 ? fma(texelFetch(height_maps, index, 0).r, params.height_range, params.height_min) : 0.0;
    if (params.background_mode == 1u) {
        float blend = flat_region_blend(world_texel / float(params.region_size));
        height = mix(height, params.ground_level, smoothstep(0.0, 1.0, blend));
    }
    return height;
}

uint terrain_control(ivec3 index) {
    // Outside loaded regions follows the auto-material path, matching the
    // raster shader's background behavior without an invalid array-layer read.
    if (params.control_maps_implicit != 0u) {
        return 1u;
    }
    return index.z >= 0 ? floatBitsToUint(texelFetch(control_maps, index, 0).r) : 1u;
}

vec2 rotate_vec2(vec2 value, vec2 cs) {
    return vec2(fma(cs.x, value.x, cs.y * value.y), fma(cs.x, value.y, -cs.y * value.x));
}

vec2 control_rotation(uint control) {
    float angle = float((control >> 10u) & 15u) * TAU_16TH;
    return vec2(cos(angle), sin(angle));
}

float control_scale(uint control) {
    return 0.9 - float((((control >> 7u) & 7u) + 3u) % 8u + 1u) * 0.1;
}

float blend_power(float value, float sharpness) {
    value = max(value, 1e-8);
    if (params.fast_blend_curve == 0u) {
        return exp2(sharpness * log2(value));
    }
    float value_2 = value * value;
    float value_4 = value_2 * value_2;
    if (sharpness < 6.0) return value_4;
    float value_8 = value_4 * value_4;
    if (sharpness < 12.0) return value_8;
    float value_16 = value_8 * value_8;
    if (sharpness < 24.0) return value_16;
    float value_32 = value_16 * value_16;
    return sharpness < 48.0 ? value_32 : value_32 * value_32;
}

float adjusted_height(float raw_height, int id, float scale_control, float scale_uv) {
    vec4 material = materials[id];
    float height_scale = (material.z * 0.04) / max(scale_control * scale_uv, 1e-8);
    float height_offset = 0.5 + material.y * height_scale - 0.5 * height_scale;
    return clamp(fma(raw_height, height_scale, height_offset), 0.0, 1.0);
}

void accumulate_material(float spatial_weight, uint control, vec2 texture_weight,
        ivec2 texture_id, inout MaterialAccumulation accumulation, vec3 vertex) {
    float scale_control = control_scale(control);
    vec2 scaled_uv = vertex.xz * scale_control;
    vec2 control_cs = control_rotation(control);
    scaled_uv = rotate_vec2(scaled_uv, control_cs);

    float sharpness = fma(60.0, params.blend_sharpness * params.displacement_sharpness, 4.0);

    if (texture_weight.x > 0.0005 && texture_id.x >= 0 && uint(texture_id.x) < params.texture_layer_count) {
        int id = texture_id.x & 31;
        float uv_scale = max(materials[id].x, 1e-8);
        vec2 sample_uv = scaled_uv * uv_scale - 0.5;
        float raw_height = textureLod(displacement_maps, vec3(sample_uv, float(id)), 0.0).r;
        float material_weight = blend_power(spatial_weight + texture_weight.x + raw_height, sharpness) * spatial_weight;
        accumulation.height = fma(adjusted_height(raw_height, id, scale_control, uv_scale), material_weight, accumulation.height);
        accumulation.total_weight += material_weight;
    }

    if (texture_weight.y > 0.0005 && texture_id.y != texture_id.x &&
            texture_id.y >= 0 && uint(texture_id.y) < params.texture_layer_count) {
        int id = texture_id.y & 31;
        float uv_scale = max(materials[id].x, 1e-8);
        vec2 sample_uv = scaled_uv * uv_scale - 0.5;
        float raw_height = textureLod(displacement_maps, vec3(sample_uv, float(id)), 0.0).r;
        float material_weight = blend_power(spatial_weight + texture_weight.y + raw_height, sharpness) * spatial_weight;
        accumulation.height = fma(adjusted_height(raw_height, id, scale_control, uv_scale), material_weight, accumulation.height);
        accumulation.total_weight += material_weight;
    }
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= int(params.width) || pixel.y >= int(params.height)) return;

    vec2 normalized = (vec2(pixel) + 0.5) / vec2(params.width, params.height);
    float strip = clamp(floor(normalized.x * params.tessellation_level), 0.0, max(params.tessellation_level - 1.0, 0.0));
    float strip_scale = exp2(strip);
    vec2 uv = (vec2(fract(normalized.x * params.tessellation_level), normalized.y) - 0.5) *
            (params.mesh_size * 2.0) / strip_scale;
    uv += round(vec2(params.target_x, params.target_z) * params.vertex_density * strip_scale) / strip_scale;

    vec2 index_id = floor(uv);
    vec2 fraction = fract(uv);
    vec2 inverse_fraction = 1.0 - fraction;
    vec4 weights = vec4(
            inverse_fraction.x * fraction.y,
            fraction.x * fraction.y,
            fraction.x * inverse_fraction.y,
            inverse_fraction.x * inverse_fraction.y);

    ivec3 index[4];
    quad_indices(index_id, index[0], index[1], index[2], index[3]);

    float h[4];
    vec3 index_normal[4];
    h[3] = terrain_height(index[3], index_id);
    h[2] = terrain_height(index[2], index_id + vec2(1.0, 0.0));
    h[0] = terrain_height(index[0], index_id + vec2(0.0, 1.0));
    index_normal[3] = normalize(vec3(h[3] - h[2], params.vertex_spacing, h[3] - h[0]));

    vec3 world_normal;
    float interpolated_height;
    if (params.sampling_mode == 2u) {
        h[1] = h[3];
        index_normal[0] = index_normal[3];
        index_normal[1] = index_normal[3];
        index_normal[2] = index_normal[3];
        world_normal = index_normal[3];
        interpolated_height = h[3];
    } else {
        h[1] = terrain_height(index[1], index_id + vec2(1.0, 1.0));
        float h4 = terrain_height(offset_index(index_id, index[3], ivec2(1, 2)), index_id + vec2(1.0, 2.0));
        float h5 = terrain_height(offset_index(index_id, index[3], ivec2(2, 1)), index_id + vec2(2.0, 1.0));
        float h6 = terrain_height(offset_index(index_id, index[3], ivec2(2, 0)), index_id + vec2(2.0, 0.0));
        float h7 = terrain_height(offset_index(index_id, index[3], ivec2(0, 2)), index_id + vec2(0.0, 2.0));
        index_normal[0] = normalize(vec3(h[0] - h[1], params.vertex_spacing, h[0] - h7));
        index_normal[1] = normalize(vec3(h[1] - h5, params.vertex_spacing, h[1] - h4));
        index_normal[2] = normalize(vec3(h[2] - h6, params.vertex_spacing, h[2] - h[1]));
        world_normal = normalize(index_normal[0] * weights[0] + index_normal[1] * weights[1] +
                index_normal[2] * weights[2] + index_normal[3] * weights[3]);
        interpolated_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
    }

    vec3 vertex = vec3(uv.x * params.vertex_spacing, interpolated_height, uv.y * params.vertex_spacing);

    uint control[4];
    if (params.sampling_mode == 2u) {
        uint sample_control = terrain_control(index[3]);
        control[0] = sample_control;
        control[1] = sample_control;
        control[2] = sample_control;
        control[3] = sample_control;
    } else {
        for (int i = 0; i < 4; ++i) control[i] = terrain_control(index[i]);
    }

    if (params.auto_enabled != 0u) {
        float auto_blend = clamp(fma(params.auto_slope * 2.0, world_normal.y - 1.0, 1.0) -
                params.auto_height_reduction * 0.01 * vertex.y, 0.0, 1.0);
        uint auto_layer_max = max(params.texture_layer_count, 1u) - 1u;
        uint authored_base_id = min(params.auto_base_texture, auto_layer_max);
        uint authored_overlay_id = min(params.auto_overlay_texture, auto_layer_max);
        bool swap_pair = authored_base_id > authored_overlay_id;
        uint auto_base_id = swap_pair ? authored_overlay_id : authored_base_id;
        uint auto_overlay_id = swap_pair ? authored_base_id : authored_overlay_id;
        float canonical_blend = swap_pair ? 1.0 - auto_blend : auto_blend;
        uint auto_control = ((auto_base_id & 31u) << 27u) |
                ((auto_overlay_id & 31u) << 22u) |
                ((uint(fma(canonical_blend, 255.0, 0.5)) & 255u) << 14u);
        for (int i = 0; i < 4; ++i) {
            if ((control[i] & 1u) != 0u || index[i].z < 0) control[i] = auto_control;
        }
    }

    bool uniform_control = params.sampling_mode == 2u ||
            (control[0] == control[1] && control[0] == control[2] && control[0] == control[3] &&
             index[0].z >= 0 && index[1].z >= 0 && index[2].z >= 0 && index[3].z >= 0);

    ivec2 texture_ids[4];
    vec2 texture_weights[4];
    for (int i = 0; i < 4; ++i) {
        texture_ids[i] = ivec2(int((control[i] >> 27u) & 31u), int((control[i] >> 22u) & 31u));
        float overlay = float((control[i] >> 14u) & 255u) * DIV_255;
        texture_weights[i] = vec2(1.0 - overlay, overlay);
    }

    bool same_pair = texture_ids[0] == texture_ids[1] && texture_ids[0] == texture_ids[2] &&
            texture_ids[0] == texture_ids[3];
    uint non_material_mask = 0x3FFFu;
    bool same_transform = (control[0] & non_material_mask) == (control[1] & non_material_mask) &&
            (control[0] & non_material_mask) == (control[2] & non_material_mask) &&
            (control[0] & non_material_mask) == (control[3] & non_material_mask);
    bool coherent_pair = params.sampling_mode != 2u && same_pair && same_transform &&
            index[0].z >= 0 && index[1].z >= 0 && index[2].z >= 0 && index[3].z >= 0;
    vec2 coherent_pair_weights = texture_weights[3];
    if (coherent_pair) {
        float overlay_weight = texture_weights[0].y * weights[0] + texture_weights[1].y * weights[1] +
                texture_weights[2].y * weights[2] + texture_weights[3].y * weights[3];
        coherent_pair_weights = vec2(1.0 - overlay_weight, overlay_weight);
    }

    if (!coherent_pair && !uniform_control && params.sampling_mode != 2u) {
        vec2 redistributed[4];
        for (int target = 0; target < 4; ++target) redistributed[target] = vec2(0.0);
        for (int source = 0; source < 4; ++source) {
            float base_weight = texture_weights[source].x * weights[source];
            float overlay_weight = texture_weights[source].y * weights[source];
            int source_base = texture_ids[source].x;
            int source_overlay = texture_ids[source].y;
            for (int target = 0; target < 4; ++target) {
                if (texture_ids[target].x == source_base) redistributed[target].x += base_weight;
                if (texture_ids[target].y == source_base) redistributed[target].y += base_weight;
                if (texture_ids[target].x == source_overlay) redistributed[target].x += overlay_weight;
                if (texture_ids[target].y == source_overlay) redistributed[target].y += overlay_weight;
            }
        }
        for (int i = 0; i < 4; ++i) texture_weights[i] = redistributed[i];
    }

    MaterialAccumulation accumulation = MaterialAccumulation(0.0, 0.0);
    if (coherent_pair) {
        accumulate_material(1.0, control[3], coherent_pair_weights, texture_ids[3], accumulation, vertex);
    } else if (params.sampling_mode == 2u || uniform_control) {
        accumulate_material(1.0, control[3], texture_weights[3], texture_ids[3], accumulation, vertex);
    } else {
        accumulate_material(weights[3], control[3], texture_weights[3], texture_ids[3], accumulation, vertex);
        accumulate_material(weights[2], control[2], texture_weights[2], texture_ids[2], accumulation, vertex);
        accumulate_material(weights[1], control[1], texture_weights[1], texture_ids[1], accumulation, vertex);
        accumulate_material(weights[0], control[0], texture_weights[0], texture_ids[0], accumulation, vertex);
    }

    float material_height = accumulation.height / max(accumulation.total_weight, 1e-8);
    vec3 encoded_offset = clamp(fma(world_normal * (material_height - 0.5) * 2.0, vec3(0.5), vec3(0.5)), 0.0, 1.0);
    imageStore(output_image, pixel, vec4(encoded_offset, 1.0));
}
