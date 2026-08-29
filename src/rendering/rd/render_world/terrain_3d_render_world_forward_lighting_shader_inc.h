// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_FORWARD_LIGHTING_SHADER_INC_H
#define TERRAIN3D_RENDER_WORLD_FORWARD_LIGHTING_SHADER_INC_H

// Generated from project/addons/terrain_3d/rd/terrain_forward_lighting_inc.glsl.
// RenderWorld deliberately shares the exact native Forward+ lighting ABI/helper implementation.
static const char *T3D_RW_FORWARD_LIGHTING_SHADER_INCLUDE = R"T3DRWGLSL(
#ifndef TERRAIN3D_FORWARD_LIGHTING_INC_GLSL
#define TERRAIN3D_FORWARD_LIGHTING_INC_GLSL

#if defined(T3D_FORWARD_NATIVE_LIGHTING) && !defined(T3D_FORWARD_DEPTH)
#extension GL_EXT_samplerless_texture_functions : enable
// Godot 4.7.2 Forward+ light-buffer ABI. Keep field order synchronized with
// servers/rendering/renderer_rd/shaders/light_data_inc.glsl.
struct T3DNativeLightData {
    vec3 position;
    float inv_radius;
    vec3 direction;
    float size;
    vec3 color;
    float attenuation;
    vec3 area_width;
    float cone_attenuation;
    vec3 area_height;
    float cone_angle;
    float specular_amount;
    float shadow_opacity;
    float pad0;
    float pad1;
    vec4 atlas_rect;
    mat4 shadow_matrix;
    float shadow_bias;
    float shadow_normal_bias;
    float transmittance_bias;
    float soft_shadow_size;
    float soft_shadow_scale;
    uint mask;
    float volumetric_fog_energy;
    uint bake_mode;
    vec4 projector_rect;
};

struct T3DNativeDirectionalLightData {
    vec3 direction;
    float energy;
    vec3 color;
    float size;
    float specular;
    uint mask;
    float softshadow_angle;
    float soft_shadow_scale;
    uint blend_splits;
    float shadow_opacity;
    float fade_from;
    float fade_to;
    uvec2 pad;
    uint bake_mode;
    float volumetric_fog_energy;
    vec4 shadow_bias;
    vec4 shadow_normal_bias;
    vec4 shadow_transmittance_bias;
    vec4 shadow_z_range;
    vec4 shadow_range_begin;
    vec4 shadow_split_offsets;
    mat4 shadow_matrix1;
    mat4 shadow_matrix2;
    mat4 shadow_matrix3;
    mat4 shadow_matrix4;
    vec2 uv_scale1;
    vec2 uv_scale2;
    vec2 uv_scale3;
    vec2 uv_scale4;
};

layout(set = 2, binding = 0, std140) uniform T3DForwardLightFrame {
    uvec4 cluster_params; // shift, width, type size, max element words
    uvec4 scene_params;   // directional count, default mask, shadow flags, directional soft-shadow flag
    vec4 depth_params;    // z near, z far, inverse z far, reserved
    uvec4 render_params;  // width, height, max cluster elements, reserved
    vec4 ambient_light_color_energy;
    vec4 environment_params0; // ambient sky mix, emissive exposure, IBL exposure, radiance pixel size
    vec4 environment_params1; // radiance octahedral border size, reserved
    mat3 radiance_inverse_xform;
}
t3d_forward_light_frame;
layout(set = 2, binding = 1, std140) uniform T3DDirectionalLights {
    T3DNativeDirectionalLightData data[8];
}
t3d_directional_lights;
layout(set = 2, binding = 2, std430) readonly buffer T3DOmniLights {
    T3DNativeLightData data[];
}
t3d_omni_lights;
layout(set = 2, binding = 3, std430) readonly buffer T3DSpotLights {
    T3DNativeLightData data[];
}
t3d_spot_lights;
layout(set = 2, binding = 4, std430) readonly buffer T3DAreaLights {
    T3DNativeLightData data[];
}
t3d_area_lights;
layout(set = 2, binding = 5, std430) readonly buffer T3DClusterBuffer {
    uint data[];
}
t3d_cluster_buffer;
layout(set = 2, binding = 6) uniform sampler t3d_shadow_sampler;
layout(set = 2, binding = 7) uniform texture2D t3d_shadow_atlas;
layout(set = 2, binding = 8) uniform texture2D t3d_directional_shadow_atlas;

// Godot 4.7.2 scene-integration ABI used by F6. These layouts mirror
// light_data_inc.glsl/decal_data_inc.glsl and are borrowed, never owned here.
struct T3DNativeReflectionData {
    vec3 box_extents;
    float index;
    vec3 box_offset;
    uint mask;
    vec3 ambient;
    float intensity;
    float blend_distance;
    uint exterior;
    uint box_project;
    uint ambient_mode;
    float exposure_normalization;
    float pad0;
    float pad1;
    float pad2;
    mat4 local_matrix;
};

struct T3DNativeDecalData {
    mat4 xform;
    vec3 inv_extents;
    float albedo_mix;
    vec4 albedo_rect;
    vec4 normal_rect;
    vec4 orm_rect;
    vec4 emission_rect;
    vec4 modulate;
    float emission_energy;
    uint mask;
    float upper_fade;
    float lower_fade;
    mat3x4 normal_xform;
    vec3 normal;
    float normal_fade;
};

layout(set = 2, binding = 9, std430) readonly buffer T3DReflectionProbes {
    T3DNativeReflectionData data[];
}
t3d_reflections;
layout(set = 2, binding = 10) uniform texture2DArray t3d_reflection_atlas;
layout(set = 2, binding = 11, std430) readonly buffer T3DDecals {
    T3DNativeDecalData data[];
}
t3d_decals;
layout(set = 2, binding = 12) uniform texture2D t3d_decal_atlas;
layout(set = 2, binding = 13) uniform texture2D t3d_decal_atlas_srgb;
layout(set = 2, binding = 14) uniform sampler t3d_scene_sampler;
#ifdef T3D_FORWARD_RADIANCE_ARRAY
layout(set = 2, binding = 15) uniform texture2DArray t3d_radiance_octmap;
#else
layout(set = 2, binding = 15) uniform texture2D t3d_radiance_octmap;
#endif

const uint T3D_SCENE_FEATURE_REFLECTIONS = 1u;
const uint T3D_SCENE_FEATURE_DECALS = 2u;
const uint T3D_SCENE_FEATURE_AMBIENT_LIGHT = 4u;
const uint T3D_SCENE_FEATURE_AMBIENT_CUBEMAP = 8u;
const uint T3D_SCENE_FEATURE_REFLECTION_CUBEMAP = 16u;
const uint T3D_SCENE_FEATURE_RADIANCE_ARRAY = 32u;
const uint T3D_CLUSTER_TYPE_DECAL = 3u;
const uint T3D_CLUSTER_TYPE_REFLECTION = 4u;

const float T3D_PI = 3.14159265358979323846;

float t3d_native_attenuation(float distance_to_light, float inv_range, float decay) {
    float nd = distance_to_light * inv_range;
    nd *= nd;
    nd *= nd;
    nd = max(1.0 - nd, 0.0);
    nd *= nd;
    return nd * pow(max(distance_to_light, 0.0001), -decay);
}

uint t3d_cluster_range_mask(uint word_index, uint item_min, uint item_max) {
    int local_min = clamp(int(item_min) - int(word_index) * 32, 0, 31);
    int mask_width = min(int(item_max) - int(item_min), 32 - local_min);
    return mask_width > 0 ? bitfieldInsert(0u, 0xffffffffu, local_min, mask_width) : 0u;
}

bool t3d_cluster_range(
    uint type_index, vec3 view_position, out uint type_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
    uint cluster_shift = t3d_forward_light_frame.cluster_params.x;
    uint cluster_width = t3d_forward_light_frame.cluster_params.y;
    uint cluster_type_size = t3d_forward_light_frame.cluster_params.z;
    uint max_words = t3d_forward_light_frame.cluster_params.w;
    if (cluster_width == 0u || cluster_type_size == 0u || max_words == 0u || t3d_forward_light_frame.depth_params.y <= 0.0) {
        return false;
    }
    uint stride = max_words + 32u;
    uint cluster_count = cluster_type_size / stride;
    uint cluster_height = cluster_count / cluster_width;
    uvec2 cluster_pos = uvec2(gl_FragCoord.xy) >> cluster_shift;
    if (cluster_pos.x >= cluster_width || cluster_pos.y >= cluster_height) {
        return false;
    }
    uint cluster_offset = (cluster_width * cluster_pos.y + cluster_pos.x) * stride;
    uint cluster_z = uint(clamp((-view_position.z * t3d_forward_light_frame.depth_params.z) * 32.0, 0.0, 31.0));
    type_offset = cluster_offset + cluster_type_size * type_index;
    uint item_min_max = t3d_cluster_buffer.data[type_offset + max_words + cluster_z];
    item_min = item_min_max & 0xffffu;
    item_max = item_min_max >> 16u;
    item_from = min(item_min >> 5u, max_words);
    item_to = min(item_max == 0u ? 0u : ((item_max - 1u) >> 5u) + 1u, max_words);
    return item_to > item_from;
}

float t3d_shadow_compare(texture2D shadow_texture, vec3 coord) {
    return texture(sampler2DShadow(shadow_texture, t3d_shadow_sampler), coord);
}

float t3d_shadow_pcf(texture2D shadow_texture, vec3 coord, float scale) {
    ivec2 size = textureSize(shadow_texture, 0);
    vec2 texel = scale / max(vec2(size), vec2(1.0));
    if (scale <= 0.0) {
        return t3d_shadow_compare(shadow_texture, coord);
    }
    float shadow = 0.0;
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2(-0.5, -0.5), coord.z));
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2(0.5, -0.5), coord.z));
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2(-0.5, 0.5), coord.z));
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2(0.5, 0.5), coord.z));
    return shadow * 0.25;
}

mat4 t3d_directional_shadow_matrix(T3DNativeDirectionalLightData light, uint cascade) {
    if (cascade == 0u) {
        return light.shadow_matrix1;
    }
    if (cascade == 1u) {
        return light.shadow_matrix2;
    }
    if (cascade == 2u) {
        return light.shadow_matrix3;
    }
    return light.shadow_matrix4;
}

float t3d_directional_component(vec4 values, uint index) {
    return index == 0u ? values.x : (index == 1u ? values.y : (index == 2u ? values.z : values.w));
}

float t3d_directional_shadow(uint index, vec3 view_position, vec3 view_normal) {
    if ((t3d_forward_light_frame.scene_params.z & 6u) != 6u) {
        return 1.0;
    }
    T3DNativeDirectionalLightData light = t3d_directional_lights.data[index];
    if (light.shadow_opacity <= 0.001) {
        return 1.0;
    }
    float depth_z = -view_position.z;
    uint cascade = depth_z < light.shadow_split_offsets.x
        ? 0u
        : (depth_z < light.shadow_split_offsets.y ? 1u : (depth_z < light.shadow_split_offsets.z ? 2u : 3u));
    vec3 base_normal_bias = view_normal * (1.0 - max(0.0, dot(light.direction, -view_normal)));
    vec4 v = vec4(view_position, 1.0);
    v.xyz += light.direction * t3d_directional_component(light.shadow_bias, cascade);
    vec3 normal_bias = base_normal_bias * t3d_directional_component(light.shadow_normal_bias, cascade);
    normal_bias -= light.direction * dot(light.direction, normal_bias);
    v.xyz += normal_bias;
    vec4 coord4 = t3d_directional_shadow_matrix(light, cascade) * v;
    if (abs(coord4.w) < 1e-7) {
        return 1.0;
    }
    vec3 coord = coord4.xyz / coord4.w;
    float filter_scale = t3d_forward_light_frame.scene_params.w != 0u ? max(light.soft_shadow_scale, 1.0) : 0.0;
    float shadow = t3d_shadow_pcf(t3d_directional_shadow_atlas, coord, filter_scale);
    if (light.fade_to > light.fade_from && depth_z > light.fade_from) {
        shadow = mix(shadow, 1.0, clamp((depth_z - light.fade_from) / (light.fade_to - light.fade_from), 0.0, 1.0));
    }
    return mix(1.0, shadow, clamp(light.shadow_opacity, 0.0, 1.0));
}

float t3d_omni_shadow(T3DNativeLightData light, vec3 view_position, vec3 view_normal) {
    if ((t3d_forward_light_frame.scene_params.z & 5u) != 5u || light.shadow_opacity <= 0.001) {
        return 1.0;
    }
    vec2 texel = 1.0 / max(vec2(textureSize(t3d_shadow_atlas, 0)), vec2(1.0));
    vec4 uv_rect = light.atlas_rect;
    uv_rect.xy += texel;
    uv_rect.zw -= texel * 2.0;
    vec2 flip_offset = light.direction.xy;
    vec3 local_vert = (light.shadow_matrix * vec4(view_position, 1.0)).xyz;
    float shadow_len = length(local_vert);
    if (shadow_len <= 1e-6) {
        return 1.0;
    }
    vec3 shadow_dir = local_vert / shadow_len;
    vec3 local_normal = normalize(mat3(light.shadow_matrix) * view_normal);
    vec3 normal_bias = local_normal * light.shadow_normal_bias * (1.0 - abs(dot(local_normal, shadow_dir)));
    vec3 shadow_sample = normalize(shadow_dir + normal_bias);
    if (shadow_sample.z >= 0.0) {
        uv_rect.xy += flip_offset;
    }
    shadow_sample.z = 1.0 + abs(shadow_sample.z);
    vec2 pos = shadow_sample.xy / shadow_sample.z;
    float depth = 1.0 - (shadow_len - light.shadow_bias) * light.inv_radius;
    pos = pos * 0.5 + 0.5;
    pos = uv_rect.xy + pos * uv_rect.zw;
    float filter_scale = light.soft_shadow_size > 0.0 ? max(light.soft_shadow_scale / max(shadow_sample.z, 0.001), 0.0) : 0.0;
    float shadow = t3d_shadow_pcf(t3d_shadow_atlas, vec3(pos, depth), filter_scale);
    return mix(1.0, shadow, clamp(light.shadow_opacity, 0.0, 1.0));
}

float t3d_spot_shadow(T3DNativeLightData light, vec3 view_position, vec3 view_normal, vec3 light_direction, float light_length) {
    if ((t3d_forward_light_frame.scene_params.z & 5u) != 5u || light.shadow_opacity <= 0.001) {
        return 1.0;
    }
    vec3 normal_bias = view_normal * light_length * light.shadow_normal_bias * (1.0 - abs(dot(view_normal, light_direction)));
    vec4 splane = light.shadow_matrix * vec4(view_position + normal_bias, 1.0);
    splane.z += light.shadow_bias;
    if (abs(splane.w) < 1e-7) {
        return 1.0;
    }
    splane /= splane.w;
    vec3 coord = vec3(splane.xy * light.atlas_rect.zw + light.atlas_rect.xy, splane.z);
    float filter_scale = light.soft_shadow_size > 0.0 ? max(light.soft_shadow_scale, 0.0) : 0.0;
    float shadow = t3d_shadow_pcf(t3d_shadow_atlas, coord, filter_scale);
    return mix(1.0, shadow, clamp(light.shadow_opacity, 0.0, 1.0));
}

float t3d_area_shadow(T3DNativeLightData light, vec3 view_position, vec3 view_normal) {
    if ((t3d_forward_light_frame.scene_params.z & 5u) != 5u || light.shadow_opacity <= 0.001) {
        return 1.0;
    }
    vec3 local_vert = (light.shadow_matrix * vec4(view_position, 1.0)).xyz;
    float shadow_len = length(local_vert);
    if (shadow_len <= 1e-6) {
        return 1.0;
    }
    vec3 shadow_dir = local_vert / shadow_len;
    vec3 local_normal = normalize(mat3(light.shadow_matrix) * view_normal);
    vec3 normal_bias = local_normal * light.shadow_normal_bias * (1.0 - abs(dot(local_normal, shadow_dir)));
    vec3 shadow_sample = normalize(shadow_dir + normal_bias);
    shadow_sample.z = 1.0 + abs(shadow_sample.z);
    vec2 pos = shadow_sample.xy / shadow_sample.z;
    vec2 texel = 1.0 / max(vec2(textureSize(t3d_shadow_atlas, 0)), vec2(1.0));
    vec4 uv_rect = light.atlas_rect;
    uv_rect.xy += texel;
    uv_rect.zw -= texel * 2.0;
    pos = uv_rect.xy + (pos * 0.5 + 0.5) * uv_rect.zw;
    float depth = 1.0 - (shadow_len - light.shadow_bias) * light.cone_attenuation;
    float filter_scale = light.soft_shadow_size > 0.0 ? max(light.soft_shadow_scale / max(shadow_sample.z, 0.001), 0.0) : 0.0;
    float shadow = t3d_shadow_pcf(t3d_shadow_atlas, vec3(pos, depth), filter_scale);
    return mix(1.0, shadow, clamp(light.shadow_opacity, 0.0, 1.0));
}

bool t3d_scene_feature(uint feature) {
    return (t3d_forward_light_frame.render_params.w & feature) != 0u;
}

bool t3d_rect_active(vec4 rect) {
    return rect.z > 0.0 && rect.w > 0.0;
}

void t3d_forward_apply_decals(vec3 view_position,
    uint surface_mask,
    inout vec3 albedo,
    inout vec3 view_normal,
    inout float roughness,
    inout float metallic,
    inout float ao,
    inout vec3 emission) {
    if (!t3d_scene_feature(T3D_SCENE_FEATURE_DECALS)) {
        return;
    }
    uint type_offset, item_min, item_max, item_from, item_to;
    if (!t3d_cluster_range(T3D_CLUSTER_TYPE_DECAL, view_position, type_offset, item_min, item_max, item_from, item_to)) {
        return;
    }
    vec3 vertex_ddx = dFdx(view_position);
    vec3 vertex_ddy = dFdy(view_position);
    for (uint word = item_from; word < item_to; ++word) {
        uint cluster_mask = t3d_cluster_buffer.data[type_offset + word] & t3d_cluster_range_mask(word, item_min, item_max);
        while (cluster_mask != 0u) {
            uint bit = uint(findMSB(cluster_mask));
            cluster_mask &= ~(1u << bit);
            uint decal_index = word * 32u + bit;
            T3DNativeDecalData decal = t3d_decals.data[decal_index];
            if ((decal.mask & surface_mask) == 0u) {
                continue;
            }
            vec3 uv_local = (decal.xform * vec4(view_position, 1.0)).xyz;
            if (any(lessThan(uv_local, vec3(0.0, -1.0, 0.0))) || any(greaterThan(uv_local, vec3(1.0)))) {
                continue;
            }
            float fade = pow(max(1.0 - abs(uv_local.y), 0.0), uv_local.y > 0.0 ? decal.upper_fade : decal.lower_fade);
            if (decal.normal_fade > 0.0) {
                fade *= smoothstep(decal.normal_fade, 1.0, dot(view_normal, decal.normal) * 0.5 + 0.5);
            }
            vec2 ddx = (decal.xform * vec4(vertex_ddx, 0.0)).xz;
            vec2 ddy = (decal.xform * vec4(vertex_ddy, 0.0)).xz;
            float material_alpha = fade;
            if (t3d_rect_active(decal.albedo_rect)) {
                vec2 uv = uv_local.xz * decal.albedo_rect.zw + decal.albedo_rect.xy;
                vec4 decal_albedo =
                    textureGrad(
                        sampler2D(t3d_decal_atlas_srgb, t3d_scene_sampler), uv, ddx * decal.albedo_rect.zw, ddy * decal.albedo_rect.zw)
                    * decal.modulate;
                decal_albedo.a *= fade;
                material_alpha = decal_albedo.a;
                albedo = mix(albedo, decal_albedo.rgb, decal_albedo.a * decal.albedo_mix);
                if (t3d_rect_active(decal.normal_rect)) {
                    vec2 normal_uv = uv_local.xz * decal.normal_rect.zw + decal.normal_rect.xy;
                    vec3 decal_normal = textureGrad(
                        sampler2D(t3d_decal_atlas, t3d_scene_sampler), normal_uv, ddx * decal.normal_rect.zw, ddy * decal.normal_rect.zw)
                                            .xyz;
                    decal_normal.xy = decal_normal.xy * vec2(2.0, -2.0) - vec2(1.0, -1.0);
                    decal_normal.z = sqrt(max(0.0, 1.0 - dot(decal_normal.xy, decal_normal.xy)));
                    decal_normal = (decal.normal_xform * decal_normal.xzy).xyz;
                    view_normal = normalize(mix(view_normal, decal_normal, decal_albedo.a));
                }
                if (t3d_rect_active(decal.orm_rect)) {
                    vec2 orm_uv = uv_local.xz * decal.orm_rect.zw + decal.orm_rect.xy;
                    vec3 decal_orm =
                        textureGrad(sampler2D(t3d_decal_atlas, t3d_scene_sampler), orm_uv, ddx * decal.orm_rect.zw, ddy * decal.orm_rect.zw)
                            .xyz;
                    ao = mix(ao, decal_orm.r, decal_albedo.a);
                    roughness = mix(roughness, decal_orm.g, decal_albedo.a);
                    metallic = mix(metallic, decal_orm.b, decal_albedo.a);
                }
            }
            if (t3d_rect_active(decal.emission_rect)) {
                vec2 emission_uv = uv_local.xz * decal.emission_rect.zw + decal.emission_rect.xy;
                emission += textureGrad(sampler2D(t3d_decal_atlas_srgb, t3d_scene_sampler),
                                emission_uv,
                                ddx * decal.emission_rect.zw,
                                ddy * decal.emission_rect.zw)
                                .rgb
                    * decal.modulate.rgb * decal.emission_energy * fade;
            }
        }
    }
}

vec2 t3d_oct_wrap(vec2 v) {
    vec2 sign_value = mix(vec2(-1.0), vec2(1.0), greaterThanEqual(v, vec2(0.0)));
    return (1.0 - abs(v.yx)) * sign_value;
}

vec2 t3d_vec3_to_oct(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    n.xy = n.z >= 0.0 ? n.xy : t3d_oct_wrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

vec2 t3d_vec3_to_oct_with_border(vec3 n) {
    // Matches RenderSceneDataRD::reflection_atlas_border_size: x is the
    // per-probe octahedral padding, y is 1 - 2*x.
    float padding = max(t3d_forward_light_frame.depth_params.w, 0.0);
    return t3d_vec3_to_oct(n) * max(1.0 - padding * 2.0, 0.0) + padding;
}

vec3 t3d_forward_reflection(
    vec3 view_position, vec3 view_normal, vec3 view_direction, float roughness, float metallic, vec3 albedo, uint surface_mask) {
    if (!t3d_scene_feature(T3D_SCENE_FEATURE_REFLECTIONS)) {
        return vec3(0.0);
    }
    uint type_offset, item_min, item_max, item_from, item_to;
    if (!t3d_cluster_range(T3D_CLUSTER_TYPE_REFLECTION, view_position, type_offset, item_min, item_max, item_from, item_to)) {
        return vec3(0.0);
    }
    vec3 reflected_view = reflect(-view_direction, view_normal);
    vec3 accumulated = vec3(0.0);
    float accumulated_weight = 0.0;
    for (uint word = item_from; word < item_to && accumulated_weight < 0.999; ++word) {
        uint cluster_mask = t3d_cluster_buffer.data[type_offset + word] & t3d_cluster_range_mask(word, item_min, item_max);
        while (cluster_mask != 0u && accumulated_weight < 0.999) {
            uint bit = uint(findMSB(cluster_mask));
            cluster_mask &= ~(1u << bit);
            uint probe_index = word * 32u + bit;
            T3DNativeReflectionData probe = t3d_reflections.data[probe_index];
            if ((probe.mask & surface_mask) == 0u || probe.intensity <= 0.0) {
                continue;
            }
            vec3 local_pos = (probe.local_matrix * vec4(view_position, 1.0)).xyz;
            if (any(greaterThan(abs(local_pos), probe.box_extents))) {
                continue;
            }
            float blend = 1.0;
            if (probe.blend_distance > 0.0) {
                vec3 axis_distance = min(vec3(probe.blend_distance), probe.box_extents);
                vec3 blend_axes = (abs(local_pos) - probe.box_extents + axis_distance) / max(axis_distance, vec3(1e-6));
                blend_axes = clamp(vec3(1.0) - blend_axes, vec3(0.0), vec3(1.0));
                blend = pow(blend_axes.x * blend_axes.y * blend_axes.z, 2.0);
            }
            float weight = max(0.0, blend - accumulated_weight);
            if (weight <= 0.0) {
                continue;
            }
            vec3 local_ref = (probe.local_matrix * vec4(reflected_view, 0.0)).xyz;
            if (probe.box_project != 0u) {
                vec3 nrdir = normalize(local_ref);
                vec3 safe_dir = vec3(abs(nrdir.x) < 1e-6 ? (nrdir.x < 0.0 ? -1e-6 : 1e-6) : nrdir.x,
                    abs(nrdir.y) < 1e-6 ? (nrdir.y < 0.0 ? -1e-6 : 1e-6) : nrdir.y,
                    abs(nrdir.z) < 1e-6 ? (nrdir.z < 0.0 ? -1e-6 : 1e-6) : nrdir.z);
                vec3 rbmax = (probe.box_extents - local_pos) / safe_dir;
                vec3 rbmin = (-probe.box_extents - local_pos) / safe_dir;
                vec3 rbminmax = mix(rbmin, rbmax, greaterThan(safe_dir, vec3(0.0)));
                float fa = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
                local_ref = local_pos + safe_dir * fa - probe.box_offset;
            }
            float roughness_lod = sqrt(clamp(roughness, 0.0, 1.0)) * 5.0;
            vec2 reflection_uv = t3d_vec3_to_oct_with_border(local_ref);
            vec3 reflected =
                textureLod(sampler2DArray(t3d_reflection_atlas, t3d_scene_sampler), vec3(reflection_uv, probe.index), roughness_lod).rgb;
            reflected *= probe.exposure_normalization * probe.intensity;
            accumulated += reflected * weight;
            accumulated_weight += weight;
        }
    }
    vec3 f0 = mix(vec3(0.04), albedo, clamp(metallic, 0.0, 1.0));
    float ndotv = clamp(dot(view_normal, view_direction), 0.0, 1.0);
    vec3 fresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - ndotv, 5.0);
    return accumulated * fresnel;
}

vec3 t3d_forward_radiance_sample(vec3 direction, float roughness) {
    vec3 radiance_direction = t3d_forward_light_frame.radiance_inverse_xform * direction;
    vec2 radiance_uv = t3d_vec3_to_oct_with_border(radiance_direction);
#ifdef T3D_FORWARD_RADIANCE_ARRAY
    float roughness_layer;
    float roughness_blend = modf(sqrt(clamp(roughness, 0.0, 1.0)) * 5.0, roughness_layer);
    vec3 sample_a = textureLod(sampler2DArray(t3d_radiance_octmap, t3d_scene_sampler), vec3(radiance_uv, roughness_layer), 0.0).rgb;
    vec3 sample_b =
        textureLod(sampler2DArray(t3d_radiance_octmap, t3d_scene_sampler), vec3(radiance_uv, min(roughness_layer + 1.0, 5.0)), 0.0).rgb;
    return mix(sample_a, sample_b, roughness_blend);
#else
    return textureLod(sampler2D(t3d_radiance_octmap, t3d_scene_sampler), radiance_uv, sqrt(clamp(roughness, 0.0, 1.0)) * 5.0).rgb;
#endif
}

void t3d_forward_environment_lighting(vec3 albedo,
    vec3 view_normal,
    vec3 view_direction,
    float roughness,
    float metallic,
    float ao,
    float specular_amount,
    out vec3 ambient_diffuse,
    out vec3 environment_specular) {
    ambient_diffuse = vec3(0.0);
    environment_specular = vec3(0.0);
    vec3 ambient_light = vec3(0.0);
    if (t3d_scene_feature(T3D_SCENE_FEATURE_AMBIENT_LIGHT)) {
        ambient_light = t3d_forward_light_frame.ambient_light_color_energy.rgb;
        if (t3d_scene_feature(T3D_SCENE_FEATURE_AMBIENT_CUBEMAP)) {
            vec3 cubemap_ambient = t3d_forward_radiance_sample(view_normal, 1.0) * t3d_forward_light_frame.environment_params0.z
                * t3d_forward_light_frame.ambient_light_color_energy.a;
            ambient_light = mix(ambient_light, cubemap_ambient, clamp(t3d_forward_light_frame.environment_params0.x, 0.0, 1.0));
        }
    }
    ambient_diffuse = ambient_light * albedo * (1.0 - clamp(metallic, 0.0, 1.0)) * ao;

    if (t3d_scene_feature(T3D_SCENE_FEATURE_REFLECTION_CUBEMAP) && specular_amount > 0.0) {
        vec3 reflected = reflect(-view_direction, view_normal);
        reflected = normalize(mix(reflected, view_normal, roughness * roughness));
        float horizon = min(1.0 + dot(reflected, view_normal), 1.0);
        vec3 f0 = mix(vec3(0.04 * specular_amount), albedo, clamp(metallic, 0.0, 1.0));
        float ndotv = clamp(dot(view_normal, view_direction), 0.0, 1.0);
        vec3 fresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - ndotv, 5.0);
        environment_specular = t3d_forward_radiance_sample(reflected, roughness) * t3d_forward_light_frame.environment_params0.z
            * t3d_forward_light_frame.ambient_light_color_energy.a * horizon * horizon * fresnel;
    }
}

void t3d_light_response(vec3 albedo,
    vec3 normal,
    vec3 view_direction,
    vec3 light_direction,
    vec3 radiance,
    float attenuation,
    float roughness,
    float metallic,
    float specular_amount,
    uint lighting_model,
    inout vec3 diffuse_light,
    inout vec3 specular_light) {
    float ndotl = max(dot(normal, light_direction), 0.0);
    if (ndotl <= 0.0 || attenuation <= 0.0) {
        return;
    }
    if (lighting_model == 1u) {
        diffuse_light += albedo * radiance * ((ndotl * attenuation) / T3D_PI);
        return;
    }
    vec3 half_direction = normalize(light_direction + view_direction);
    float gloss = max(1.0 - roughness, 0.0);
    float spec_power = mix(4.0, 128.0, gloss * gloss);
    float specular_term = pow(max(dot(normal, half_direction), 0.0), spec_power) * gloss * max(specular_amount, 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    if (lighting_model == 2u) {
        diffuse_light += albedo * (ndotl / T3D_PI) * (1.0 - metallic * 0.6) * radiance * attenuation;
        specular_light += f0 * specular_term * radiance * attenuation;
        return;
    }
    float fresnel = pow(1.0 - max(dot(normal, view_direction), 0.0), 5.0);
    vec3 F = mix(f0, vec3(1.0), fresnel);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    diffuse_light += kd * albedo / T3D_PI * radiance * (ndotl * attenuation);
    specular_light += F * specular_term * radiance * (ndotl * attenuation);
}

void t3d_forward_direct_lighting(vec3 albedo,
    vec3 world_position,
    vec3 world_normal,
    mat4 view_matrix,
    vec3 camera_world,
    float roughness,
    float metallic,
    float material_specular_scale,
    uint surface_mask,
    uint lighting_model,
    bool receive_shadows,
    uint directional_shadowmask_mode,
    float directional_shadow_visibility,
    out vec3 diffuse_light,
    out vec3 specular_light) {
    diffuse_light = vec3(0.0);
    specular_light = vec3(0.0);
    if (lighting_model == 0u) {
        return;
    }
    vec3 view_position = (view_matrix * vec4(world_position, 1.0)).xyz;
    vec3 view_normal = normalize(mat3(view_matrix) * world_normal);
    vec3 view_direction = normalize(-view_position);

    uint directional_count = min(t3d_forward_light_frame.scene_params.x, 8u);
    for (uint i = 0u; i < directional_count; ++i) {
        T3DNativeDirectionalLightData light = t3d_directional_lights.data[i];
        if ((light.mask & surface_mask) == 0u) {
            continue;
        }
        float shadow = receive_shadows ? t3d_directional_shadow(i, view_position, view_normal) : 1.0;
        if (i == 0u && directional_shadowmask_mode != 0u) {
            float baked_visibility = clamp(directional_shadow_visibility, 0.0, 1.0);
            shadow = directional_shadowmask_mode == 1u ? baked_visibility : shadow * baked_visibility;
        }
        t3d_light_response(albedo,
            view_normal,
            view_direction,
            normalize(light.direction),
            light.color * light.energy,
            shadow,
            roughness,
            metallic,
            light.specular * material_specular_scale,
            lighting_model,
            diffuse_light,
            specular_light);
    }

    for (uint light_type = 0u; light_type < 3u; ++light_type) {
        uint type_offset, item_min, item_max, item_from, item_to;
        if (!t3d_cluster_range(light_type, view_position, type_offset, item_min, item_max, item_from, item_to)) {
            continue;
        }
        for (uint word = item_from; word < item_to; ++word) {
            uint mask = t3d_cluster_buffer.data[type_offset + word] & t3d_cluster_range_mask(word, item_min, item_max);
            while (mask != 0u) {
                uint bit = uint(findMSB(mask));
                mask &= ~(1u << bit);
                uint light_index = word * 32u + bit;
                T3DNativeLightData light = light_type == 0u
                    ? t3d_omni_lights.data[light_index]
                    : (light_type == 1u ? t3d_spot_lights.data[light_index] : t3d_area_lights.data[light_index]);
                if ((light.mask & surface_mask) == 0u) {
                    continue;
                }
                vec3 to_light = light.position - view_position;
                float distance_to_light = length(to_light);
                if (distance_to_light <= 1e-6) {
                    continue;
                }
                vec3 light_direction = to_light / distance_to_light;
                float attenuation;
                float shadow = 1.0;
                if (light_type == 0u) {
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                    if (receive_shadows) {
                        shadow = t3d_omni_shadow(light, view_position, view_normal);
                    }
                } else if (light_type == 1u) {
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                    float cone_angle = light.cone_angle;
                    float scos = max(dot(-light_direction, light.direction), cone_angle);
                    float spot_rim = max(1e-4, (1.0 - scos) / max(1.0 - cone_angle, 1e-4));
                    attenuation *= 1.0 - pow(spot_rim, light.cone_attenuation);
                    if (receive_shadows) {
                        shadow = t3d_spot_shadow(light, view_position, view_normal, light_direction, distance_to_light);
                    }
                } else {
                    // Godot area lights are one-sided rectangles. Use the same native width/height vectors and
                    // closest-point distance as Forward+ while keeping Terrain3D's simplified direct BRDF.
                    if (dot(light.direction, view_position - light.position) <= 0.0) {
                        continue;
                    }
                    float width_length = length(light.area_width);
                    float height_length = length(light.area_height);
                    if (width_length <= 1e-6 || height_length <= 1e-6) {
                        continue;
                    }
                    vec3 width_direction = light.area_width / width_length;
                    vec3 height_direction = light.area_height / height_length;
                    vec3 from_center = view_position - light.position;
                    float local_x = clamp(dot(from_center, width_direction), -0.5 * width_length, 0.5 * width_length);
                    float local_y = clamp(dot(from_center, height_direction), -0.5 * height_length, 0.5 * height_length);
                    vec3 closest_point = light.position + width_direction * local_x + height_direction * local_y;
                    to_light = closest_point - view_position;
                    distance_to_light = length(to_light);
                    if (distance_to_light <= 1e-6) {
                        continue;
                    }
                    light_direction = to_light / distance_to_light;
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                    // LTC is intentionally not duplicated; projected rectangle area supplies the far-field energy scale.
                    float area = max(width_length * height_length, 0.0001);
                    attenuation *= clamp(area / max(area, distance_to_light * distance_to_light), 0.0, 1.0);
                    if (receive_shadows) {
                        shadow = t3d_area_shadow(light, view_position, view_normal);
                    }
                }
                t3d_light_response(albedo,
                    view_normal,
                    view_direction,
                    light_direction,
                    light.color,
                    attenuation * shadow,
                    roughness,
                    metallic,
                    light.specular_amount * material_specular_scale,
                    lighting_model,
                    diffuse_light,
                    specular_light);
            }
        }
    }
}

#endif // T3D_FORWARD_NATIVE_LIGHTING && !T3D_FORWARD_DEPTH
#endif // TERRAIN3D_FORWARD_LIGHTING_INC_GLSL
)T3DRWGLSL";

#endif // TERRAIN3D_RENDER_WORLD_FORWARD_LIGHTING_SHADER_INC_H
