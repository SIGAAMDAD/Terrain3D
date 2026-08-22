#[vertex]
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in float in_part_code;

layout(set = 0, binding = 0, std140) uniform FrameData {
    mat4 view_projection;
    vec4 light_direction_ambient;
    vec4 light_color;
    vec4 terrain_params; // region size, map size, height min, height range
    vec4 mesh_params;    // vertex spacing, mesh size, debug blend, material layer count
    vec4 target_params;  // target x, target z, subdivision, reserved
} frame_data;

layout(set = 0, binding = 1, std430) readonly buffer RegionMapData {
    int region_map[];
};
layout(set = 0, binding = 2) uniform sampler2DArray height_maps;

layout(push_constant, std430) uniform DrawData {
    vec2 origin;
    float scale;
    float lod;
    float tier;
    float stitch_x;
    float stitch_z;
    float flags;
} draw_data;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;

int floor_div(int value, int divisor) {
    int q = value / divisor;
    int r = value - q * divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}

int floor_mod(int value, int divisor) {
    return value - floor_div(value, divisor) * divisor;
}

int region_layer(ivec2 region_cell) {
    int map_size = int(frame_data.terrain_params.y + 0.5);
    ivec2 map_pos = region_cell + ivec2(map_size / 2);
    if (map_pos.x < 0 || map_pos.y < 0 || map_pos.x >= map_size || map_pos.y >= map_size) {
        return -1;
    }
    return region_map[map_pos.y * map_size + map_pos.x] - 1;
}

float decode_height(float encoded) {
    return fma(encoded, frame_data.terrain_params.w, frame_data.terrain_params.z);
}

float sample_height_texel(ivec2 texel) {
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 region_cell = ivec2(floor_div(texel.x, region_size), floor_div(texel.y, region_size));
    int layer = region_layer(region_cell);
    if (layer < 0) {
        return frame_data.terrain_params.z;
    }
    ivec2 local = ivec2(floor_mod(texel.x, region_size), floor_mod(texel.y, region_size));
    ivec3 size3 = textureSize(height_maps, 0);
    local = clamp(local, ivec2(0), size3.xy - ivec2(1));
    return decode_height(texelFetch(height_maps, ivec3(local, layer), 0).r);
}

float sample_height_nearest(vec2 world_xz) {
    vec2 world_texel = world_xz / frame_data.mesh_params.x;
    return sample_height_texel(ivec2(round(world_texel)));
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

float edge_axis_offset(int edge_index, int test_axis) {
    float mesh_size = frame_data.mesh_params.y;
    if (edge_index == 0) {
        return test_axis == 2 ? -(4.0 * mesh_size + 6.0) : 0.0;
    }
    if (test_axis == 0) {
        return 4.0 * mesh_size + 8.0;
    }
    return test_axis == 2 ? -2.0 : 0.0;
}

vec2 edge_offset(float part_code) {
    int part = int(part_code + 0.5);
    int sx = clamp(int(draw_data.stitch_x + 0.5), 0, 2);
    int sz = clamp(int(draw_data.stitch_z + 0.5), 0, 2);
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
    float morph = smoothstep(0.0, 1.0,
        (max_xz * inv_scale - frame_data.mesh_params.y - 4.0) / denominator);
    vec2 vertex_fract = fract(grid_xz * 0.5) * 2.0;
    vec2 shift;
    float subdiv = max(frame_data.target_params.z, 1.0);
    if (scale < frame_data.mesh_params.x / subdiv + 1e-6) {
        float alternate = round(fract(
            round(mod(world_xz.y * inv_scale, 4.0)) *
            round(mod(world_xz.x * inv_scale, 4.0)) * 0.25));
        shift = mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y), alternate);
    } else {
        shift = vertex_fract * round((fract(world_xz * 0.25 * inv_scale) - 0.5) * 4.0);
    }
    return vec3(shift, morph);
}

void main() {
    vec2 grid = in_position.xz + edge_offset(in_part_code);
    vec2 start_world_xz = grid * draw_data.scale + draw_data.origin;
    vec3 morph = clipmap_morph(start_world_xz, grid, draw_data.scale);
    vec2 shift_world = morph.xy * draw_data.scale;
    vec2 end_world_xz = start_world_xz - shift_world;
    vec2 world_xz = mix(start_world_xz, end_world_xz, morph.z);

    float height;
    if (draw_data.scale < frame_data.mesh_params.x) {
        height = sample_height_interpolated(world_xz);
    } else {
        height = mix(sample_height_nearest(start_world_xz), sample_height_nearest(end_world_xz), morph.z);
    }

    float normal_step = max(frame_data.mesh_params.x, draw_data.scale);
    float h_l = sample_height_nearest(world_xz - vec2(normal_step, 0.0));
    float h_r = sample_height_nearest(world_xz + vec2(normal_step, 0.0));
    float h_d = sample_height_nearest(world_xz - vec2(0.0, normal_step));
    float h_u = sample_height_nearest(world_xz + vec2(0.0, normal_step));
    world_normal = normalize(vec3(h_l - h_r, 2.0 * normal_step, h_d - h_u));
    world_position = vec3(world_xz.x, height, world_xz.y);
    gl_Position = frame_data.view_projection * vec4(world_position, 1.0);
}

#[fragment]
#version 450

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform FrameData {
    mat4 view_projection;
    vec4 light_direction_ambient;
    vec4 light_color;
    vec4 terrain_params;
    vec4 mesh_params;
    vec4 target_params;
} frame_data;

layout(set = 0, binding = 1, std430) readonly buffer RegionMapData {
    int region_map[];
};
layout(set = 0, binding = 3) uniform sampler2DArray control_maps;
layout(set = 0, binding = 4) uniform sampler2DArray color_maps;
layout(set = 0, binding = 5) uniform sampler2DArray albedo_maps;

int fragment_floor_div(int value, int divisor) {
    int q = value / divisor;
    int r = value - q * divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}

int fragment_floor_mod(int value, int divisor) {
    return value - fragment_floor_div(value, divisor) * divisor;
}

int fragment_region_layer(ivec2 region_cell) {
    int map_size = int(frame_data.terrain_params.y + 0.5);
    ivec2 map_pos = region_cell + ivec2(map_size / 2);
    if (map_pos.x < 0 || map_pos.y < 0 || map_pos.x >= map_size || map_pos.y >= map_size) {
        return -1;
    }
    return region_map[map_pos.y * map_size + map_pos.x] - 1;
}

ivec2 fragment_control_texel(vec2 world_xz) {
    return ivec2(floor(world_xz / frame_data.mesh_params.x));
}

int fragment_layer_for_texel(ivec2 texel) {
    int region_size = int(frame_data.terrain_params.x + 0.5);
    return fragment_region_layer(ivec2(
        fragment_floor_div(texel.x, region_size),
        fragment_floor_div(texel.y, region_size)));
}

uint fragment_control(ivec2 texel, int layer) {
    if (layer < 0) {
        return 0u;
    }
    int region_size = int(frame_data.terrain_params.x + 0.5);
    ivec2 local = ivec2(
        fragment_floor_mod(texel.x, region_size),
        fragment_floor_mod(texel.y, region_size));
    ivec3 size3 = textureSize(control_maps, 0);
    local = clamp(local, ivec2(0), size3.xy - ivec2(1));
    return floatBitsToUint(texelFetch(control_maps, ivec3(local, layer), 0).r);
}

vec4 fragment_color_texel(ivec2 local_coord, ivec2 region_cell, ivec2 map_size) {
    ivec2 region_delta = ivec2(0);
    if (local_coord.x < 0) {
        local_coord.x += map_size.x;
        region_delta.x = -1;
    } else if (local_coord.x >= map_size.x) {
        local_coord.x -= map_size.x;
        region_delta.x = 1;
    }
    if (local_coord.y < 0) {
        local_coord.y += map_size.y;
        region_delta.y = -1;
    } else if (local_coord.y >= map_size.y) {
        local_coord.y -= map_size.y;
        region_delta.y = 1;
    }
    int layer = fragment_region_layer(region_cell + region_delta);
    return layer >= 0 ? texelFetch(color_maps, ivec3(local_coord, layer), 0) : vec4(1.0);
}

vec4 fragment_color(vec2 world_xz) {
    float region_size = frame_data.terrain_params.x;
    vec2 world_texel = world_xz / frame_data.mesh_params.x;
    vec2 world_region_uv = (world_texel + vec2(0.5)) / region_size;
    vec2 region_pos = floor(world_region_uv);
    vec2 local_uv = world_region_uv - region_pos;
    ivec2 map_size = textureSize(color_maps, 0).xy;
    vec2 color_texel = local_uv * vec2(map_size) - vec2(0.5);
    ivec2 base = ivec2(floor(color_texel));
    vec2 f = fract(color_texel);
    ivec2 region_cell = ivec2(region_pos);
    vec4 c00 = fragment_color_texel(base, region_cell, map_size);
    vec4 c10 = fragment_color_texel(base + ivec2(1, 0), region_cell, map_size);
    vec4 c01 = fragment_color_texel(base + ivec2(0, 1), region_cell, map_size);
    vec4 c11 = fragment_color_texel(base + ivec2(1, 1), region_cell, map_size);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

vec2 rotate_control_uv(vec2 uv, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return vec2(fma(c, uv.x, s * uv.y), fma(c, uv.y, -s * uv.x));
}

vec3 sample_material_albedo(uint control, vec2 world_xz) {
    int texture_layers = max(textureSize(albedo_maps, 0).z, 1);
    int authored_layers = max(int(frame_data.mesh_params.w + 0.5), 1);
    int layer_count = min(texture_layers, authored_layers);
    int base_id = clamp(int((control >> 27u) & 0x1Fu), 0, layer_count - 1);
    int overlay_id = clamp(int((control >> 22u) & 0x1Fu), 0, layer_count - 1);
    float blend = float((control >> 14u) & 0xFFu) * (1.0 / 255.0);
    float angle = float((control >> 10u) & 0xFu) * -0.392699081698724;
    float control_scale = 0.9 - float((((control >> 7u) & 0x7u) + 3u) % 8u + 1u) * 0.1;

    // Terrain3DTextureAsset defaults to 0.1 UV scale. Per-asset UV scales and
    // runtime logical->physical material remapping are part of the next RD
    // material-state pass; this direct alpha path now matches the canonical
    // control-map scale/rotation convention instead of applying its reciprocal.
    vec2 uv = rotate_control_uv(world_xz * (control_scale * 0.1), angle);
    vec3 base = texture(albedo_maps, vec3(uv, float(base_id))).rgb;
    if (overlay_id == base_id || blend <= 0.001) {
        return base;
    }
    vec3 overlay = texture(albedo_maps, vec3(uv, float(overlay_id))).rgb;
    return mix(base, overlay, smoothstep(0.12, 0.88, blend));
}

void main() {
    ivec2 control_texel = fragment_control_texel(world_position.xz);
    int layer = fragment_layer_for_texel(control_texel);
    if (layer < 0) {
        discard;
    }

    uint control = fragment_control(control_texel, layer);
    if (((control >> 2u) & 1u) != 0u) {
        discard;
    }

    vec4 terrain_color = fragment_color(world_position.xz);
    vec3 material_albedo = sample_material_albedo(control, world_position.xz);
    vec3 n = normalize(world_normal);
    vec3 light_dir = normalize(-frame_data.light_direction_ambient.xyz);
    float ndl = max(dot(n, light_dir), 0.0);
    float ambient = frame_data.light_direction_ambient.w;
    vec3 painted_albedo = material_albedo * terrain_color.rgb;
    vec3 lit = painted_albedo * (vec3(ambient) + frame_data.light_color.rgb * ndl);
    out_color = vec4(lit, 1.0);
}
