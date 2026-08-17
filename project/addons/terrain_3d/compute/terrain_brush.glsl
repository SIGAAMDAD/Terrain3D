#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct BrushElement {
    uint value;
    uint alpha_bits;
    uint target_bits;
    uint flags;
};

layout(set = 0, binding = 0, std430) restrict buffer BrushElements {
    BrushElement elements[];
};

layout(push_constant, std430) uniform Params {
    uint map_type;
    uint tool;
    uint operation;
    uint count;
    float strength;
    float selected_height;
    float brush_center_height;
    float roughness;
    uint color_rgba8;
    uint asset_id;
    uint uv_rotation;
    uint uv_scale;
    uint feature_flags;
} params;

const uint TYPE_HEIGHT = 0u;
const uint TYPE_CONTROL = 1u;
const uint TYPE_COLOR = 2u;
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

const uint FEATURE_ALT = 1u << 0u;
const uint FEATURE_CENTER_VALID = 1u << 1u;
const uint FEATURE_TEXTURE = 1u << 2u;
const uint FEATURE_ANGLE = 1u << 3u;
const uint FEATURE_SCALE = 1u << 4u;

uint get_base(uint value) { return (value >> 27u) & 31u; }
uint get_overlay(uint value) { return (value >> 22u) & 31u; }
uint get_blend(uint value) { return (value >> 14u) & 255u; }
uint get_rotation(uint value) { return (value >> 10u) & 15u; }
uint get_scale(uint value) { return (value >> 7u) & 7u; }
bool get_hole(uint value) { return ((value >> 2u) & 1u) != 0u; }
bool get_nav(uint value) { return ((value >> 1u) & 1u) != 0u; }
bool get_auto(uint value) { return (value & 1u) != 0u; }
uint encode_control(uint base_id, uint overlay_id, uint blend, uint rotation, uint scale, bool hole, bool nav, bool automatic) {
    return ((base_id & 31u) << 27u) | ((overlay_id & 31u) << 22u) |
        ((blend & 255u) << 14u) | ((rotation & 15u) << 10u) |
        ((scale & 7u) << 7u) | (hole ? 4u : 0u) | (nav ? 2u : 0u) | (automatic ? 1u : 0u);
}

void process_height(inout BrushElement e, float alpha) {
    float source = uintBitsToFloat(e.value);
    if (isnan(source) || isinf(source)) {
        source = 0.0;
    }
    float result = source;
    float amount = alpha * params.strength;
    if (params.operation == OP_ADD) {
        if (params.tool == TOOL_HEIGHT) {
            result = mix(source, params.selected_height, clamp(amount, 0.0, 1.0));
        } else if ((params.feature_flags & FEATURE_ALT) != 0u && (params.feature_flags & FEATURE_CENTER_VALID) != 0u) {
            float center = params.brush_center_height + amount;
            result = clamp(center, source, source + amount);
        } else {
            result = source + amount;
        }
    } else if (params.operation == OP_SUBTRACT) {
        if (params.tool == TOOL_HEIGHT) {
            result = mix(source, params.selected_height, clamp(amount, 0.0, 1.0));
        } else if ((params.feature_flags & FEATURE_ALT) != 0u && (params.feature_flags & FEATURE_CENTER_VALID) != 0u) {
            float center = params.brush_center_height - amount;
            result = clamp(center, source - amount, source);
        } else {
            result = source - amount;
        }
    } else if (params.operation == OP_GRADIENT) {
        result = mix(source, uintBitsToFloat(e.target_bits), clamp(amount, 0.0, 1.0));
    }
    e.value = floatBitsToUint(result);
}

void process_control(inout BrushElement e, float alpha) {
    uint base_id = get_base(e.value);
    uint overlay_id = get_overlay(e.value);
    float blend = float(get_blend(e.value)) / 255.0;
    uint rotation = get_rotation(e.value);
    uint scale = get_scale(e.value);
    bool hole = get_hole(e.value);
    bool nav = get_nav(e.value);
    bool automatic = get_auto(e.value);

    if (params.tool == TOOL_TEXTURE) {
        if (params.operation == OP_REPLACE) {
            if (alpha > 0.5) {
                if ((params.feature_flags & FEATURE_TEXTURE) != 0u) {
                    base_id = params.asset_id;
                    overlay_id = params.asset_id;
                    blend = 0.0;
                    automatic = false;
                }
                if (base_id == params.asset_id && !automatic) {
                    if ((params.feature_flags & FEATURE_ANGLE) != 0u) rotation = params.uv_rotation;
                    if ((params.feature_flags & FEATURE_SCALE) != 0u) scale = params.uv_scale;
                }
            }
        } else if (params.operation == OP_ADD) {
            float spray = clamp(params.strength * 0.05, 0.004, 0.25);
            float brush_value = clamp(alpha * spray, 0.0, 1.0);
            if ((params.feature_flags & FEATURE_TEXTURE) != 0u && alpha * params.strength * 11.0 > 0.1) {
				automatic = false;
                if (params.asset_id != base_id && params.asset_id != overlay_id) {
                    if ((params.feature_flags & FEATURE_ALT) != 0u) {
                        if (blend < 0.5) overlay_id = params.asset_id; else base_id = params.asset_id;
                    } else {
                        blend = blend >= 0.5 ? clamp(blend + brush_value, 0.0, 1.0) : clamp(blend - brush_value, 0.0, 1.0);
                        if (blend <= 1.0 / 254.0) overlay_id = params.asset_id;
                        else if (blend >= 1.0 - 1.0 / 254.0) base_id = params.asset_id;
                    }
                }
                if (base_id == params.asset_id) {
                    blend = clamp(blend - brush_value, 0.0, 1.0);
                    if (alpha > 0.5 && blend < 0.5) automatic = false;
                }
                if (overlay_id == params.asset_id) {
                    blend = clamp(blend + brush_value, 0.0, 1.0);
                    if (alpha > 0.5 && blend >= 0.5) automatic = false;
                }
            }
            if (((base_id == params.asset_id && blend < 0.5) || (overlay_id == params.asset_id && blend >= 0.5)) && !automatic && alpha > 0.5) {
                if ((params.feature_flags & FEATURE_ANGLE) != 0u) rotation = params.uv_rotation;
                if ((params.feature_flags & FEATURE_SCALE) != 0u) scale = params.uv_scale;
            }
        } else if (params.operation == OP_SUBTRACT) {
            float spray = clamp(params.strength * 0.05, 0.004, 0.25);
            float brush_value = clamp(alpha * spray, 0.0, 1.0);
            if (base_id == params.asset_id) blend = clamp(blend + brush_value, 0.0, 1.0);
            if (overlay_id == params.asset_id) blend = clamp(blend - brush_value, 0.0, 1.0);
        }
    } else if (params.tool == TOOL_AUTOSHADER && alpha > 0.5) {
        automatic = params.operation == OP_ADD;
        scale = 0u;
        rotation = 0u;
    } else if (params.tool == TOOL_HOLES && alpha > 0.5) {
        hole = params.operation == OP_ADD;
    } else if (params.tool == TOOL_NAVIGATION && alpha > 0.5) {
        nav = params.operation == OP_ADD;
    }

    uint blend_u8 = uint(clamp(round(blend * 255.0), 0.0, 255.0));
    e.value = encode_control(base_id, overlay_id, blend_u8, rotation, scale, hole, nav, automatic);
}

void process_color(inout BrushElement e, float alpha) {
    vec4 source = unpackUnorm4x8(e.value);
    float weight = clamp(alpha * params.strength, 0.0, 1.0);
    if (params.tool == TOOL_COLOR) {
        vec4 target = params.operation == OP_ADD ? unpackUnorm4x8(params.color_rgba8) : vec4(1.0);
        vec4 result = mix(source, target, weight);
        result.a = source.a;
        e.value = packUnorm4x8(result);
    } else if (params.tool == TOOL_ROUGHNESS) {
        float target = params.operation == OP_ADD ? 0.5 + 0.5 * params.roughness : 0.5;
        source.a = mix(source.a, target, weight);
        source.a = float(int(source.a * 255.0)) / 255.0;
        e.value = packUnorm4x8(source);
    }
}

void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= params.count) return;
    BrushElement e = elements[index];
    if ((e.flags & 1u) == 0u) return;
    float alpha = uintBitsToFloat(e.alpha_bits);
    if (params.map_type == TYPE_HEIGHT) process_height(e, alpha);
    else if (params.map_type == TYPE_CONTROL) process_control(e, alpha);
    else if (params.map_type == TYPE_COLOR) process_color(e, alpha);
    elements[index] = e;
}
