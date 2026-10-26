#[versions]
color_native = "";
color_prepared = "#define T3D_OCEAN_PREPARED_FRAGMENT";
shadow = "#define T3D_OCEAN_SHADOW";

#[vertex]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES

#include "terrain_strip_topology_inc.glslinc"

layout(set = 0, binding = 0, std140) uniform OceanFrame {
    mat4 view_projection[2];
    mat4 view_matrix;
    vec4 camera_time;
    vec4 light_direction;
    vec4 light_color;
    vec4 mesh_params;
    vec4 target_time;
    vec4 water_color;
    vec4 depth_params;
    vec4 wave_params;
    vec4 surface_params;
    vec4 foam_params0;
    vec4 foam_params1;
    vec4 scatter_params;
    vec4 mode_params;
}
frame_data;

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
layout(set = 0, binding = 1, std430) readonly buffer PatchBuffer {
    PatchData patches[];
};
layout(set = 0, binding = 2, std430) readonly buffer OceanStripPartBuffer {
    uint ocean_strip_part_words[];
};
layout(push_constant, std430) uniform OceanPush {
    uint patch_base;
    uint reserved0;
    uint reserved1;
    uint reserved2;
}
push_data;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec2 ocean_uv;
layout(location = 2) out float view_distance;

uint t3d_view_index() {
    return min(uint(gl_ViewIndex), 1u);
}
float hash21(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p))) * uvec2(1597334673u, 3812015801u);
    uint n = (q.x ^ q.y) * 1597334673u;
    return float(n) * (1.0 / float(0xffffffffu));
}
float noise21(vec2 p) {
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1, 0)), c = hash21(i + vec2(0, 1)), d = hash21(i + vec2(1));
    return 2.0 * mix(mix(a, b, u.x), mix(c, d, u.x), u.y) - 1.0;
}
float octave(vec2 uv) {
    uv += noise21(uv * 0.5) * 2.0;
    vec2 wave = 1.0 - abs(sin(uv));
    float value = wave.x * wave.y + 1e-6;
    return 1.0 - 2.0 * (value * inversesqrt(value)) + value;
}
float ocean_height(vec2 pos, int iterations) {
    float freq = 0.03 * frame_data.wave_params.z;
    float amp = 4.0;
    vec2 uv = pos;
    float height = 0.0;
    float time = frame_data.target_time.w * frame_data.wave_params.x;
    for (int i = 0; i < 4; i++) {
        if (i >= iterations)
            break;
        height += (octave((uv + time) * freq) + octave((uv - time) * freq)) * amp;
        uv = mat2(vec2(1.8, 0.8), vec2(-0.8, 1.8)) * uv;
        freq *= 1.9;
        amp *= 0.2;
    }
    return height * 0.1 * frame_data.wave_params.y;
}
float edge_axis_offset(int edge_index, int test_axis) {
    float mesh_size = frame_data.mesh_params.x;
    if (edge_index == 0)
        return test_axis == 2 ? -(4.0 * mesh_size + 6.0) : 0.0;
    if (test_axis == 0)
        return 4.0 * mesh_size + 8.0;
    return test_axis == 2 ? -2.0 : 0.0;
}
vec2 edge_offset(uint part_code, PatchData patch_data) {
    int part = int(part_code);
    int sx = clamp(int(patch_data.stitch_x), 0, 2), sz = clamp(int(patch_data.stitch_z), 0, 2);
    if (part == 1)
        return vec2(edge_axis_offset(0, sx), -2.0 * float(sz - 1));
    if (part == 2)
        return vec2(edge_axis_offset(1, sx), -2.0 * float(sz - 1));
    if (part == 3)
        return vec2(0.0, edge_axis_offset(0, sz));
    if (part == 4)
        return vec2(0.0, edge_axis_offset(1, sz));
    return vec2(0.0);
}
vec3 clipmap_morph(vec2 world_xz, vec2 grid_xz, float scale) {
    float inv_scale = 1.0 / max(scale, 1e-6);
    vec2 target = frame_data.target_time.xz;
    float max_xz = max(abs(world_xz.x - target.x), abs(world_xz.y - target.y));
    float denom = max(frame_data.mesh_params.x - 4.0, 1.0);
    float morph = smoothstep(0.0, 1.0, (max_xz * inv_scale - frame_data.mesh_params.x - 4.0) / denom);
    vec2 vf = fract(grid_xz * 0.5) * 2.0;
    vec2 shift;
    float subdiv = max(frame_data.mesh_params.z, 1.0);
    if (scale < frame_data.mesh_params.y / subdiv + 1e-6) {
        float alt = round(fract(round(mod(world_xz.y * inv_scale, 4.0)) * round(mod(world_xz.x * inv_scale, 4.0)) * 0.25));
        shift = mix(vf, vec2(vf.x, -vf.y), alt);
    } else {
        shift = vf * round((fract(world_xz * 0.25 * inv_scale) - 0.5) * 4.0);
    }
    return vec3(shift, morph);
}

void main() {
    uint encoded_vertex = uint(gl_VertexIndex);
    uint part_slot = encoded_vertex >> T3D_CLIPMAP_STRIP_VERTEX_ID_SHIFT;
    uint local_vertex = encoded_vertex & T3D_CLIPMAP_STRIP_VERTEX_ID_MASK;
    uint descriptor_index = push_data.reserved1 + part_slot;
    uint word_base = descriptor_index * T3D_CLIPMAP_STRIP_PART_WORD_COUNT;
    T3DClipmapStripPart part = t3d_clipmap_strip_part_decode(
        ocean_strip_part_words[word_base + 0u],
        ocean_strip_part_words[word_base + 1u],
        ocean_strip_part_words[word_base + 2u]);
    uint actual_vertex_count = t3d_terrain_strip_actual_vertex_count(part.cell_width, part.cell_height);
    T3DTerrainStripVertex strip_vertex = t3d_terrain_strip_vertex_hot(
        local_vertex,
        part.cell_width,
        actual_vertex_count,
        part.part_code,
        part.flags & T3D_CLIPMAP_STRIP_FLAG_SERPENTINE_ROWS,
        0u);
    uint patch_index = push_data.patch_base + uint(gl_InstanceIndex);
    PatchData patch_data = patches[patch_index];
    vec2 grid = vec2(strip_vertex.grid + part.origin) + edge_offset(part.part_code, patch_data);
    vec2 start_xz = grid * patch_data.scale + patch_data.origin;
    vec3 morph = clipmap_morph(start_xz, grid, patch_data.scale);
    vec2 world_xz = mix(start_xz, start_xz - morph.xy * patch_data.scale, morph.z);
    int vertex_iterations = clamp(int(frame_data.mode_params.x + 0.5), 0, 4);
    float y = frame_data.mesh_params.w + ocean_height(world_xz, vertex_iterations);
    world_position = vec3(world_xz.x, y, world_xz.y);
    ocean_uv = world_xz;
    view_distance = length(world_position - frame_data.camera_time.xyz);
    gl_Position = frame_data.view_projection[t3d_view_index()] * vec4(world_position, 1.0);
}

#[fragment]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES

layout(set = 0, binding = 0, std140) uniform OceanFrame {
    mat4 view_projection[2];
    mat4 view_matrix;
    vec4 camera_time;
    vec4 light_direction;
    vec4 light_color;
    vec4 mesh_params;
    vec4 target_time;
    vec4 water_color;
    vec4 depth_params;
    vec4 wave_params;
    vec4 surface_params;
    vec4 foam_params0;
    vec4 foam_params1;
    vec4 scatter_params;
    vec4 mode_params;
}
frame_data;
layout(location = 0) in vec3 world_position;
layout(location = 1) in vec2 ocean_uv;
layout(location = 2) in float view_distance;
#ifndef T3D_OCEAN_SHADOW
layout(location = 0) out vec4 out_color;
#endif

float hash21(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p))) * uvec2(1597334673u, 3812015801u);
    uint n = (q.x ^ q.y) * 1597334673u;
    return float(n) * (1.0 / float(0xffffffffu));
}
float noise21(vec2 p) {
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1, 0)), c = hash21(i + vec2(0, 1)), d = hash21(i + vec2(1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void main() {
#ifndef T3D_OCEAN_SHADOW
    vec3 dx = dFdx(world_position), dy = dFdy(world_position);
    vec3 normal = normalize(cross(dy, dx));
    if (normal.y < 0.0)
        normal = -normal;
    vec3 view_dir = normalize(frame_data.camera_time.xyz - world_position);
    // P53: Fresnel amplitude at zero or below makes the entire expression
    // zero; avoid a per-pixel transcendental in those authored water modes.
    float fresnel = 0.0;
    if (frame_data.wave_params.w > 0.0) {
        fresnel = clamp(frame_data.wave_params.w
            * pow(max(1.0 - dot(normal, view_dir), 0.0), max(frame_data.surface_params.x, 0.01)), 0.0, 1.0);
    }
#ifdef T3D_OCEAN_PREPARED_FRAGMENT
    vec3 light_dir = frame_data.scatter_params.xyz;
#else
    vec3 light_dir = normalize(-frame_data.light_direction.xyz);
#endif
    float ndl = max(dot(normal, light_dir), 0.0);
    vec3 color = frame_data.water_color.rgb * (0.35 + 0.65 * ndl);
    int refraction_mode = clamp(int(frame_data.mode_params.z + 0.5), 0, 2);
    if (refraction_mode > 0)
        color = mix(color * 0.55, color, clamp(frame_data.depth_params.y, 0.0, 1.0));
    if (refraction_mode > 1)
        color = mix(color, vec3(dot(color, vec3(0.333))), clamp(frame_data.depth_params.w * 2.0, 0.0, 0.3));
    int packed = int(frame_data.mode_params.w + 0.5);
    int foam_mode = packed & 3;
    int scatter_mode = (packed >> 2) & 3;
    int shading_mode = (packed >> 4) & 3;
    if (shading_mode > 0 && frame_data.surface_params.y > 0.0) {
        // P61: a zero-facing highlight raised to an exponent >= 8 is zero.
        // Skip pow on back-facing light/view configurations.
        float highlight = max(dot(reflect(-light_dir, normal), view_dir), 0.0);
        if (highlight > 0.0) {
            float spec = pow(highlight, mix(8.0, 96.0, 1.0 - fresnel)) * frame_data.surface_params.y;
            color += vec3(spec);
        }
    }
    if (scatter_mode > 0 && frame_data.foam_params0.w != 0.0) {
        float scatter = max(normal.y - frame_data.foam_params1.x, 0.0) * frame_data.foam_params0.w * 0.03;
        color += frame_data.water_color.rgb * scatter;
    }
    if (foam_mode > 0) {
        // P53: no visible foam means no hash/noise evaluation. Both the
        // crest and the view fade are nonnegative by construction.
        float crest = clamp((1.0 - normal.y) * frame_data.surface_params.w - frame_data.surface_params.z * 0.1, 0.0, 1.0);
        if (crest > 0.0) {
            // P61: do not calculate distance fade for completely flat water.
            float fade = 1.0 - smoothstep(0.0, max(frame_data.foam_params0.x, 1.0), view_distance);
            if (fade > 0.0) {
#ifdef T3D_OCEAN_PREPARED_FRAGMENT
                float n = noise21(ocean_uv * frame_data.scatter_params.w + frame_data.target_time.w * 0.05);
#else
                float n = noise21(ocean_uv * 0.1 * max(frame_data.foam_params0.z, 0.1) + frame_data.target_time.w * 0.05);
#endif
                float foam = crest * n * fade;
                color = mix(color, vec3(0.8), clamp(foam, 0.0, 1.0));
            }
        }
    }
    color = mix(color, vec3(0.45, 0.62, 0.78), fresnel * 0.35);
    float alpha = clamp(frame_data.water_color.a, 0.0, 1.0);
    out_color = vec4(color, alpha);
#endif
}
