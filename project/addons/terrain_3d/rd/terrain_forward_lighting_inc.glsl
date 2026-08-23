#ifndef TERRAIN3D_FORWARD_LIGHTING_INC_GLSL
#define TERRAIN3D_FORWARD_LIGHTING_INC_GLSL

#if defined(T3D_FORWARD_NATIVE_LIGHTING) && !defined(T3D_FORWARD_DEPTH)
#extension GL_EXT_samplerless_texture_functions : enable
// Godot 4.7.1 Forward+ light-buffer ABI. Keep field order synchronized with
// servers/rendering/renderer_rd/shaders/light_data_inc.glsl.
struct T3DNativeLightData {
    vec3 position; float inv_radius;
    vec3 direction; float size;
    vec3 color; float attenuation;
    vec3 area_width; float cone_attenuation;
    vec3 area_height; float cone_angle;
    float specular_amount; float shadow_opacity; float pad0; float pad1;
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
    vec3 direction; float energy;
    vec3 color; float size;
    float specular; uint mask; float softshadow_angle; float soft_shadow_scale;
    uint blend_splits; float shadow_opacity; float fade_from; float fade_to;
    uvec2 pad; uint bake_mode; float volumetric_fog_energy;
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
} t3d_forward_light_frame;
layout(set = 2, binding = 1, std140) uniform T3DDirectionalLights { T3DNativeDirectionalLightData data[8]; } t3d_directional_lights;
layout(set = 2, binding = 2, std430) readonly buffer T3DOmniLights { T3DNativeLightData data[]; } t3d_omni_lights;
layout(set = 2, binding = 3, std430) readonly buffer T3DSpotLights { T3DNativeLightData data[]; } t3d_spot_lights;
layout(set = 2, binding = 4, std430) readonly buffer T3DAreaLights { T3DNativeLightData data[]; } t3d_area_lights;
layout(set = 2, binding = 5, std430) readonly buffer T3DClusterBuffer { uint data[]; } t3d_cluster_buffer;
layout(set = 2, binding = 6) uniform sampler t3d_shadow_sampler;
layout(set = 2, binding = 7) uniform texture2D t3d_shadow_atlas;
layout(set = 2, binding = 8) uniform texture2D t3d_directional_shadow_atlas;

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

bool t3d_cluster_range(uint type_index, vec3 view_position, out uint type_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
    uint cluster_shift = t3d_forward_light_frame.cluster_params.x;
    uint cluster_width = t3d_forward_light_frame.cluster_params.y;
    uint cluster_type_size = t3d_forward_light_frame.cluster_params.z;
    uint max_words = t3d_forward_light_frame.cluster_params.w;
    if (cluster_width == 0u || cluster_type_size == 0u || max_words == 0u || t3d_forward_light_frame.depth_params.y <= 0.0) return false;
    uint stride = max_words + 32u;
    uint cluster_count = cluster_type_size / stride;
    uint cluster_height = cluster_count / cluster_width;
    uvec2 cluster_pos = uvec2(gl_FragCoord.xy) >> cluster_shift;
    if (cluster_pos.x >= cluster_width || cluster_pos.y >= cluster_height) return false;
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
    if (scale <= 0.0) return t3d_shadow_compare(shadow_texture, coord);
    float shadow = 0.0;
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2(-0.5, -0.5), coord.z));
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2( 0.5, -0.5), coord.z));
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2(-0.5,  0.5), coord.z));
    shadow += t3d_shadow_compare(shadow_texture, vec3(coord.xy + texel * vec2( 0.5,  0.5), coord.z));
    return shadow * 0.25;
}

mat4 t3d_directional_shadow_matrix(T3DNativeDirectionalLightData light, uint cascade) {
    if (cascade == 0u) return light.shadow_matrix1;
    if (cascade == 1u) return light.shadow_matrix2;
    if (cascade == 2u) return light.shadow_matrix3;
    return light.shadow_matrix4;
}

float t3d_directional_component(vec4 values, uint index) {
    return index == 0u ? values.x : (index == 1u ? values.y : (index == 2u ? values.z : values.w));
}

float t3d_directional_shadow(uint index, vec3 view_position, vec3 view_normal) {
    if ((t3d_forward_light_frame.scene_params.z & 6u) != 6u) return 1.0;
    T3DNativeDirectionalLightData light = t3d_directional_lights.data[index];
    if (light.shadow_opacity <= 0.001) return 1.0;
    float depth_z = -view_position.z;
    uint cascade = depth_z < light.shadow_split_offsets.x ? 0u : (depth_z < light.shadow_split_offsets.y ? 1u : (depth_z < light.shadow_split_offsets.z ? 2u : 3u));
    vec3 base_normal_bias = view_normal * (1.0 - max(0.0, dot(light.direction, -view_normal)));
    vec4 v = vec4(view_position, 1.0);
    v.xyz += light.direction * t3d_directional_component(light.shadow_bias, cascade);
    vec3 normal_bias = base_normal_bias * t3d_directional_component(light.shadow_normal_bias, cascade);
    normal_bias -= light.direction * dot(light.direction, normal_bias);
    v.xyz += normal_bias;
    vec4 coord4 = t3d_directional_shadow_matrix(light, cascade) * v;
    if (abs(coord4.w) < 1e-7) return 1.0;
    vec3 coord = coord4.xyz / coord4.w;
    float filter_scale = t3d_forward_light_frame.scene_params.w != 0u ? max(light.soft_shadow_scale, 1.0) : 0.0;
    float shadow = t3d_shadow_pcf(t3d_directional_shadow_atlas, coord, filter_scale);
    if (light.fade_to > light.fade_from && depth_z > light.fade_from) shadow = mix(shadow, 1.0, clamp((depth_z - light.fade_from) / (light.fade_to - light.fade_from), 0.0, 1.0));
    return mix(1.0, shadow, clamp(light.shadow_opacity, 0.0, 1.0));
}

float t3d_omni_shadow(T3DNativeLightData light, vec3 view_position, vec3 view_normal) {
    if ((t3d_forward_light_frame.scene_params.z & 5u) != 5u || light.shadow_opacity <= 0.001) return 1.0;
    vec2 texel = 1.0 / max(vec2(textureSize(t3d_shadow_atlas, 0)), vec2(1.0));
    vec4 uv_rect = light.atlas_rect;
    uv_rect.xy += texel;
    uv_rect.zw -= texel * 2.0;
    vec2 flip_offset = light.direction.xy;
    vec3 local_vert = (light.shadow_matrix * vec4(view_position, 1.0)).xyz;
    float shadow_len = length(local_vert);
    if (shadow_len <= 1e-6) return 1.0;
    vec3 shadow_dir = local_vert / shadow_len;
    vec3 local_normal = normalize(mat3(light.shadow_matrix) * view_normal);
    vec3 normal_bias = local_normal * light.shadow_normal_bias * (1.0 - abs(dot(local_normal, shadow_dir)));
    vec3 shadow_sample = normalize(shadow_dir + normal_bias);
    if (shadow_sample.z >= 0.0) uv_rect.xy += flip_offset;
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
    if ((t3d_forward_light_frame.scene_params.z & 5u) != 5u || light.shadow_opacity <= 0.001) return 1.0;
    vec3 normal_bias = view_normal * light_length * light.shadow_normal_bias * (1.0 - abs(dot(view_normal, light_direction)));
    vec4 splane = light.shadow_matrix * vec4(view_position + normal_bias, 1.0);
    splane.z += light.shadow_bias;
    if (abs(splane.w) < 1e-7) return 1.0;
    splane /= splane.w;
    vec3 coord = vec3(splane.xy * light.atlas_rect.zw + light.atlas_rect.xy, splane.z);
    float filter_scale = light.soft_shadow_size > 0.0 ? max(light.soft_shadow_scale, 0.0) : 0.0;
    float shadow = t3d_shadow_pcf(t3d_shadow_atlas, coord, filter_scale);
    return mix(1.0, shadow, clamp(light.shadow_opacity, 0.0, 1.0));
}

float t3d_area_shadow(T3DNativeLightData light, vec3 view_position, vec3 view_normal) {
    if ((t3d_forward_light_frame.scene_params.z & 5u) != 5u || light.shadow_opacity <= 0.001) return 1.0;
    vec3 local_vert = (light.shadow_matrix * vec4(view_position, 1.0)).xyz;
    float shadow_len = length(local_vert);
    if (shadow_len <= 1e-6) return 1.0;
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

vec3 t3d_light_response(vec3 albedo, vec3 normal, vec3 view_direction, vec3 light_direction, vec3 radiance,
        float attenuation, float roughness, float metallic, float specular_amount, uint lighting_model) {
    float ndotl = max(dot(normal, light_direction), 0.0);
    if (ndotl <= 0.0 || attenuation <= 0.0) return vec3(0.0);
    if (lighting_model == 1u) return albedo * radiance * (ndotl * attenuation);
    vec3 half_direction = normalize(light_direction + view_direction);
    float gloss = max(1.0 - roughness, 0.0);
    float spec_power = mix(4.0, 128.0, gloss * gloss);
    float specular_term = pow(max(dot(normal, half_direction), 0.0), spec_power) * gloss * max(specular_amount, 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    if (lighting_model == 2u) {
        return (albedo * ndotl * (1.0 - metallic * 0.6) + f0 * specular_term) * radiance * attenuation;
    }
    float fresnel = pow(1.0 - max(dot(normal, view_direction), 0.0), 5.0);
    vec3 F = mix(f0, vec3(1.0), fresnel);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    return (kd * albedo / T3D_PI + F * specular_term) * radiance * (ndotl * attenuation);
}

vec3 t3d_forward_direct_lighting(vec3 albedo, vec3 world_position, vec3 world_normal, mat4 view_matrix, vec3 camera_world,
        float roughness, float metallic, float material_specular_scale, uint surface_mask, uint lighting_model, bool receive_shadows) {
    if (lighting_model == 0u) return vec3(0.0);
    vec3 view_position = (view_matrix * vec4(world_position, 1.0)).xyz;
    vec3 view_normal = normalize(mat3(view_matrix) * world_normal);
    vec3 view_direction = normalize(-view_position);
    vec3 direct = vec3(0.0);

    uint directional_count = min(t3d_forward_light_frame.scene_params.x, 8u);
    for (uint i = 0u; i < directional_count; ++i) {
        T3DNativeDirectionalLightData light = t3d_directional_lights.data[i];
        if ((light.mask & surface_mask) == 0u) continue;
        float shadow = receive_shadows ? t3d_directional_shadow(i, view_position, view_normal) : 1.0;
        direct += t3d_light_response(albedo, view_normal, view_direction, normalize(light.direction), light.color * light.energy,
                shadow, roughness, metallic, light.specular * material_specular_scale, lighting_model);
    }

    for (uint light_type = 0u; light_type < 3u; ++light_type) {
        uint type_offset, item_min, item_max, item_from, item_to;
        if (!t3d_cluster_range(light_type, view_position, type_offset, item_min, item_max, item_from, item_to)) continue;
        for (uint word = item_from; word < item_to; ++word) {
            uint mask = t3d_cluster_buffer.data[type_offset + word] & t3d_cluster_range_mask(word, item_min, item_max);
            while (mask != 0u) {
                uint bit = uint(findMSB(mask));
                mask &= ~(1u << bit);
                uint light_index = word * 32u + bit;
                T3DNativeLightData light = light_type == 0u ? t3d_omni_lights.data[light_index] :
                        (light_type == 1u ? t3d_spot_lights.data[light_index] : t3d_area_lights.data[light_index]);
                if ((light.mask & surface_mask) == 0u) continue;
                vec3 to_light = light.position - view_position;
                float distance_to_light = length(to_light);
                if (distance_to_light <= 1e-6) continue;
                vec3 light_direction = to_light / distance_to_light;
                float attenuation;
                float shadow = 1.0;
                if (light_type == 0u) {
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                    if (receive_shadows) shadow = t3d_omni_shadow(light, view_position, view_normal);
                } else if (light_type == 1u) {
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                    float cone_angle = light.cone_angle;
                    float scos = max(dot(-light_direction, light.direction), cone_angle);
                    float spot_rim = max(1e-4, (1.0 - scos) / max(1.0 - cone_angle, 1e-4));
                    attenuation *= 1.0 - pow(spot_rim, light.cone_attenuation);
                    if (receive_shadows) shadow = t3d_spot_shadow(light, view_position, view_normal, light_direction, distance_to_light);
                } else {
                    // Godot area lights are one-sided rectangles. Use the same native width/height vectors and
                    // closest-point distance as Forward+ while keeping Terrain3D's simplified direct BRDF.
                    if (dot(light.direction, view_position - light.position) <= 0.0) continue;
                    float width_length = length(light.area_width);
                    float height_length = length(light.area_height);
                    if (width_length <= 1e-6 || height_length <= 1e-6) continue;
                    vec3 width_direction = light.area_width / width_length;
                    vec3 height_direction = light.area_height / height_length;
                    vec3 from_center = view_position - light.position;
                    float local_x = clamp(dot(from_center, width_direction), -0.5 * width_length, 0.5 * width_length);
                    float local_y = clamp(dot(from_center, height_direction), -0.5 * height_length, 0.5 * height_length);
                    vec3 closest_point = light.position + width_direction * local_x + height_direction * local_y;
                    to_light = closest_point - view_position;
                    distance_to_light = length(to_light);
                    if (distance_to_light <= 1e-6) continue;
                    light_direction = to_light / distance_to_light;
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                    // LTC is intentionally not duplicated; projected rectangle area supplies the far-field energy scale.
                    float area = max(width_length * height_length, 0.0001);
                    attenuation *= clamp(area / max(area, distance_to_light * distance_to_light), 0.0, 1.0);
                    if (receive_shadows) shadow = t3d_area_shadow(light, view_position, view_normal);
                }
                direct += t3d_light_response(albedo, view_normal, view_direction, light_direction, light.color,
                        attenuation * shadow, roughness, metallic, light.specular_amount * material_specular_scale, lighting_model);
            }
        }
    }
    return direct;
}

#endif // T3D_FORWARD_NATIVE_LIGHTING && !T3D_FORWARD_DEPTH
#endif // TERRAIN3D_FORWARD_LIGHTING_INC_GLSL
