#[compute]
#version 450
// Editor-only global RD tile brush. No local-device submission or readback.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer WorkingBytes { uint words[]; };
struct Tile { ivec2 region; uint byte_offset; uint encoding; uvec4 rect; };
layout(set = 0, binding = 1, std430) readonly buffer TileTable { Tile tiles[256]; uint lookup[65536]; };
layout(set = 0, binding = 2, std430) readonly buffer BrushMask { uint mask_alpha_bits[]; };
layout(push_constant, std430) uniform Push {
    uint tile_count, map_type, tool, operation;
    float center_x, center_z, brush_size, strength;
    float rotation_cos, rotation_sin, vertex_spacing, selected_height;
    uint mask_width, mask_height, flags, color_rgba8;
    float roughness, center_height;
    uint asset_id, uv_rotation, uv_scale, reserved0, reserved1, reserved2;
    float gradient_x1, gradient_y1, gradient_z1;
    float gradient_x2, gradient_y2, gradient_z2;
    float motion_x, motion_z;
} p;
const uint TOOL_SCULPT = 1u;
const uint TOOL_HEIGHT = 2u;
const uint TOOL_TEXTURE = 3u;
const uint TOOL_COLOR = 4u;
const uint TOOL_ROUGHNESS = 5u;
const uint TOOL_AUTOSHADER = 6u;
const uint TOOL_HOLES = 7u;
const uint TOOL_NAVIGATION = 8u;
const uint OP_ADD = 0u;
const uint OP_SUBTRACT = 1u;
const uint OP_REPLACE = 2u;
const uint OP_GRADIENT = 4u;
const uint TYPE_HEIGHT = 0u;
const uint TYPE_CONTROL = 1u;
const uint TYPE_COLOR = 2u;
const float HEIGHT_MIN = -384.0;
const float HEIGHT_RANGE = 1152.0;

float brush_alpha(ivec2 texel, Tile tile) {
    if (p.mask_width == 0u || p.mask_height == 0u || p.vertex_spacing <= 0.0 || p.brush_size <= 0.0) return 0.0;
    vec2 global_texel = vec2(tile.region * 128 + texel);
    float first_x = (p.center_x - p.brush_size * 0.5 + p.vertex_spacing * 0.5) / p.vertex_spacing;
    float first_y = (p.center_z - p.brush_size * 0.5 + p.vertex_spacing * 0.5) / p.vertex_spacing;
    vec2 index = ceil(global_texel - vec2(first_x, first_y) - vec2(0.000001));
    float count = max(1.0, ceil(p.brush_size / p.vertex_spacing));
    if (any(lessThan(index, vec2(0.0))) || any(greaterThanEqual(index, vec2(count)))) return 0.0;
    vec2 offset = index * p.vertex_spacing - vec2(p.brush_size * 0.5 - p.vertex_spacing * 0.5);
    vec2 uv = vec2(offset.x * p.rotation_cos - offset.y * p.rotation_sin,
                   offset.x * p.rotation_sin + offset.y * p.rotation_cos) / p.brush_size + vec2(0.5);
    uv = clamp(uv, vec2(0.0), vec2(1.0));
    ivec2 pixel = ivec2(uv * vec2(p.mask_width, p.mask_height));
    if (any(greaterThanEqual(pixel, ivec2(p.mask_width, p.mask_height)))) return 0.0;
    float value = uintBitsToFloat(mask_alpha_bits[pixel.y * int(p.mask_width) + pixel.x]);
    return isnan(value) ? 0.0 : value;
}
uint control_brush(uint word, float alpha) {
    if (alpha <= 0.00001) return word;
    uint base = (word >> 27u) & 31u;
    uint overlay = (word >> 22u) & 31u;
    float blend = float((word >> 14u) & 255u) / 255.0;
    uint rotation = (word >> 10u) & 15u;
    uint scale = (word >> 7u) & 7u;
    bool hole = (word & 4u) != 0u;
    bool nav = (word & 2u) != 0u;
    bool automatic = (word & 1u) != 0u;
    if (p.tool == TOOL_TEXTURE) {
        if (p.operation == OP_REPLACE && alpha > 0.5) {
            if ((p.flags & 4u) != 0u) {
                base = p.asset_id;
                overlay = p.asset_id;
                blend = 0.0;
                automatic = false;
            }
            if (base == p.asset_id && !automatic) {
                if ((p.flags & 8u) != 0u) rotation = p.uv_rotation;
                if ((p.flags & 16u) != 0u) scale = p.uv_scale;
            }
        } else if (p.operation == OP_ADD) {
            float spray = clamp(p.strength * 0.05, 0.004, 0.25);
            float amount = clamp(alpha * spray, 0.0, 1.0);
            if ((p.flags & 4u) != 0u && alpha * p.strength * 11.0 > 0.1) {
                automatic = false;
                if (p.asset_id != base && p.asset_id != overlay) {
                    blend = blend >= 0.5 ? clamp(blend + amount, 0.0, 1.0) : clamp(blend - amount, 0.0, 1.0);
                    if (blend <= 1.0 / 254.0) overlay = p.asset_id;
                    else if (blend >= 1.0 - 1.0 / 254.0) base = p.asset_id;
                }
                if (base == p.asset_id) {
                    blend = clamp(blend - amount, 0.0, 1.0);
                    if (alpha > 0.5 && blend < 0.5) automatic = false;
                }
                if (overlay == p.asset_id) {
                    blend = clamp(blend + amount, 0.0, 1.0);
                    if (alpha > 0.5 && blend >= 0.5) automatic = false;
                }
            }
            if (((base == p.asset_id && blend < 0.5) || (overlay == p.asset_id && blend >= 0.5)) &&
                !automatic && alpha > 0.5) {
                if ((p.flags & 8u) != 0u) rotation = p.uv_rotation;
                if ((p.flags & 16u) != 0u) scale = p.uv_scale;
            }
        } else if (p.operation == OP_SUBTRACT) {
            float amount = clamp(alpha * clamp(p.strength * 0.05, 0.004, 0.25), 0.0, 1.0);
            if (base == p.asset_id) blend = clamp(blend + amount, 0.0, 1.0);
            if (overlay == p.asset_id) blend = clamp(blend - amount, 0.0, 1.0);
        }
    } else if (p.tool == TOOL_AUTOSHADER && alpha > 0.5) {
        automatic = p.operation == OP_ADD;
        scale = 0u;
        rotation = 0u;
    } else if (p.tool == TOOL_HOLES && alpha > 0.5) {
        hole = p.operation == OP_ADD;
    } else if (p.tool == TOOL_NAVIGATION && alpha > 0.5) {
        nav = p.operation == OP_ADD;
    }
    uint blend_u8 = uint(clamp(round(blend * 255.0), 0.0, 255.0));
    return ((base & 31u) << 27u) | ((overlay & 31u) << 22u) |
        ((blend_u8 & 255u) << 14u) | ((rotation & 15u) << 10u) | ((scale & 7u) << 7u) |
        (hole ? 4u : 0u) | (nav ? 2u : 0u) | (automatic ? 1u : 0u);
}

float decode_height(uint h) { return HEIGHT_MIN + float(h) * (HEIGHT_RANGE / 65535.0); }
uint encode_height(float h) { return uint(round(clamp((h - HEIGHT_MIN) / HEIGHT_RANGE, 0.0, 1.0) * 65535.0)); }
uint height_brush(uint half, float alpha, ivec2 texel, Tile tile) {
    if (alpha <= 0.00001) return half;
    float src = decode_height(half);
    float amount = alpha * p.strength;
    float dest = src;
    if (p.tool == TOOL_HEIGHT) dest = mix(src, p.selected_height, clamp(amount, 0.0, 1.0));
    else if (p.operation == OP_ADD) dest = src + amount;
    else if (p.operation == OP_SUBTRACT) dest = src - amount;
    else if (p.operation == OP_GRADIENT) {
        vec2 first = vec2(p.gradient_x1, p.gradient_z1);
        vec2 direction = vec2(p.gradient_x2, p.gradient_z2) - first;
        float length_squared = dot(direction, direction);
        if (length_squared < 0.01) return half;
        float sample_x = (p.center_x - p.brush_size * 0.5 + p.vertex_spacing * 0.5) / p.vertex_spacing;
        float sample_z = (p.center_z - p.brush_size * 0.5 + p.vertex_spacing * 0.5) / p.vertex_spacing;
        vec2 global_texel = vec2(tile.region * 128 + texel);
        vec2 sample_index = ceil(global_texel - vec2(sample_x, sample_z) - vec2(0.000001));
        vec2 offset = sample_index * p.vertex_spacing - vec2(p.brush_size * 0.5 - p.vertex_spacing * 0.5);
        vec2 sample_pos = vec2(p.center_x, p.center_z) + offset;
        vec2 motion = vec2(p.motion_x, p.motion_z);
        float motion_squared = dot(motion, motion);
        if (motion_squared > 0.0) {
            vec2 along = motion * inversesqrt(motion_squared);
            sample_pos = vec2(p.center_x, p.center_z) + along * dot(offset, along);
        }
        float weight = clamp(dot(direction, sample_pos - first) / length_squared, 0.0, 1.0);
        float gradient_height = mix(p.gradient_y1, p.gradient_y2, weight);
        dest = mix(src, gradient_height, clamp(amount, 0.0, 1.0));
    }
    return encode_height(dest);
}
uint color_brush(uint word, float alpha) {
    if (alpha <= 0.00001) return word;
    vec4 src = unpackUnorm4x8(word);
    float weight = clamp(alpha * p.strength, 0.0, 1.0);
    if (p.tool == TOOL_COLOR) {
        vec4 target = p.operation == OP_ADD ? unpackUnorm4x8(p.color_rgba8) : vec4(1.0);
        src.rgb = mix(src.rgb, target.rgb, weight);
    } else if (p.tool == TOOL_ROUGHNESS) {
        float target = p.operation == OP_ADD ? 0.5 + 0.5 * p.roughness : 0.5;
        src.a = float(int(mix(src.a, target, weight) * 255.0)) / 255.0;
    }
    return packUnorm4x8(src);
}
void main() {
    uint tile_id = gl_WorkGroupID.y;
    if (tile_id >= p.tile_count) return;
    Tile tile = tiles[tile_id];
    if (tile.encoding != p.map_type) return;
    uint pixel_id = gl_GlobalInvocationID.x;
    uint pixels = tile.rect.z * tile.rect.w;
    if (p.map_type == TYPE_HEIGHT) {
        uint pair = pixel_id * 2u;
        if (pair >= pixels) return;
        // Height rows are native R16; each work item owns a complete uint word.
        uint base = tile.byte_offset / 4u + pair / 2u;
        uint w = words[base];
        ivec2 p0 = ivec2(int(pair % tile.rect.z), int(pair / tile.rect.z)) + ivec2(tile.rect.xy);
        uint lo = height_brush(w & 65535u, brush_alpha(p0, tile), p0, tile);
        uint hi = (w >> 16u) & 65535u;
        if (pair + 1u < pixels) {
            ivec2 p1 = ivec2(int((pair + 1u) % tile.rect.z), int((pair + 1u) / tile.rect.z)) + ivec2(tile.rect.xy);
            hi = height_brush(hi, brush_alpha(p1, tile), p1, tile);
        }
        words[base] = lo | (hi << 16u);
        return;
    }
    if (pixel_id >= pixels) return;
    ivec2 texel = ivec2(int(pixel_id % tile.rect.z), int(pixel_id / tile.rect.z)) + ivec2(tile.rect.xy);
    float alpha = brush_alpha(texel, tile);
    uint word_index = tile.byte_offset / 4u + pixel_id;
    uint word = words[word_index];
    if (p.map_type == TYPE_CONTROL) word = control_brush(word, alpha);
    else if (p.map_type == TYPE_COLOR) word = color_brush(word, alpha);
    words[word_index] = word;
}
