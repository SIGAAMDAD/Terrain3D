#[versions]
color = "";
bringup = "#define T3D_INSTANCE_BRINGUP";
color_native = "#define T3D_FORWARD_NATIVE_LIGHTING";
color_specular = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
color_motion = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
color_specular_motion = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
color_native_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_motion_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_motion_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
depth = "#define T3D_FORWARD_DEPTH";
depth_normal_roughness = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS";
depth_normal_roughness_voxel_gi = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI";

#[vertex]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec4 in_normal;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec2 in_uv;
layout(location = 5) in vec2 in_uv2;

layout(set = 0, binding = 0, std140) uniform InstanceFrame {
    mat4 view_projection[2];
    mat4 view_matrix;
    mat4 previous_view_projection[2];
    vec4 camera_position[2];
    vec4 motion_jitter;
    vec4 motion_params;
}
frame_data;

uint t3d_view_index() {
    return min(uint(gl_ViewIndex), 1u);
}

struct InstanceRecord {
    vec3 position;
    uint quaternion01;
    uint quaternion23;
    uint scale01;
    uint scale2_flags;
    uint color_rgba8;
};
layout(set = 0, binding = 1, std430) readonly buffer InstanceRecords {
    InstanceRecord records[];
};
layout(set = 0, binding = 2, std430) readonly buffer VisibleIndices {
    uint visible_indices[];
};
#ifndef T3D_INSTANCE_BRINGUP
layout(set = 0, binding = 4, std430) readonly buffer PreviousInstanceRecords {
    InstanceRecord previous_records[];
};
#endif

layout(push_constant, std430) uniform InstancePush {
    uint material_index;
    uint overlay_material_index;
    uint visible_base;
    uint flags;
    uint visibility_layers;
    uint reserved0;
    uint reserved1;
    uint reserved2;
}
push_data;

layout(location = 0) out vec3 vertex_normal;
layout(location = 1) out vec4 vertex_tangent;
layout(location = 2) out vec4 mesh_vertex_color;
layout(location = 3) out vec4 instance_color;
layout(location = 4) out vec2 vertex_uv;
layout(location = 5) out vec3 vertex_world_position;
#ifdef T3D_FORWARD_MOTION
layout(location = 6) out vec4 current_screen_position;
layout(location = 7) out vec4 previous_screen_position;
#endif

vec3 rotate_quaternion(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec3 instance_world_position(InstanceRecord record, vec3 local_vertex) {
    vec2 q01 = unpackSnorm2x16(record.quaternion01);
    vec2 q23 = unpackSnorm2x16(record.quaternion23);
    vec4 quaternion = normalize(vec4(q01.x, q01.y, q23.x, q23.y));
    vec2 scale01 = unpackHalf2x16(record.scale01);
    vec2 scale2_flags = unpackHalf2x16(record.scale2_flags & 0x0000ffffu);
    return record.position + rotate_quaternion(quaternion, local_vertex * vec3(scale01, scale2_flags.x));
}

void main() {
    uint record_index = visible_indices[push_data.visible_base + uint(gl_InstanceIndex)];
    InstanceRecord record = records[record_index];
    vec2 q01 = unpackSnorm2x16(record.quaternion01);
    vec2 q23 = unpackSnorm2x16(record.quaternion23);
    vec4 quaternion = normalize(vec4(q01.x, q01.y, q23.x, q23.y));
    vec2 scale01 = unpackHalf2x16(record.scale01);
    vec2 scale2_flags = unpackHalf2x16(record.scale2_flags & 0x0000ffffu);
    vec3 scale = vec3(scale01, scale2_flags.x);
    vec3 local_position = in_position * scale;
    vec3 world_position = record.position + rotate_quaternion(quaternion, local_position);

    vec3 safe_scale = max(abs(scale), vec3(0.00001));
    vec3 normal = normalize(rotate_quaternion(quaternion, in_normal.xyz / safe_scale));
    vec3 tangent = rotate_quaternion(quaternion, in_tangent.xyz * scale);
    tangent = tangent - normal * dot(normal, tangent);
    tangent = normalize(tangent);
    float handedness = in_tangent.w * (scale.x * scale.y * scale.z < 0.0 ? -1.0 : 1.0);

    vertex_normal = normal;
    vertex_tangent = vec4(tangent, handedness);
    mesh_vertex_color = in_color;
    instance_color = unpackUnorm4x8(record.color_rgba8);
    vertex_uv = in_uv;
    vertex_world_position = world_position;
    vec4 current_clip = frame_data.view_projection[t3d_view_index()] * vec4(world_position, 1.0);
    gl_Position = current_clip;
#ifdef T3D_FORWARD_MOTION
    current_screen_position = current_clip;
    vec3 previous_world_position = instance_world_position(previous_records[record_index], in_position);
    previous_screen_position = frame_data.motion_params.x > 0.5
        ? frame_data.previous_view_projection[t3d_view_index()] * vec4(previous_world_position, 1.0)
        : current_clip;
#endif
}

#[fragment]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES
#ifndef T3D_INSTANCE_BRINGUP
#include "terrain_forward_lighting_inc.glsl"
#endif

layout(set = 0, binding = 0, std140) uniform InstanceFrameFragment {
    mat4 view_projection[2];
    mat4 view_matrix;
    mat4 previous_view_projection[2];
    vec4 camera_position[2];
    vec4 motion_jitter;
    vec4 motion_params;
}
frame_data;

uint t3d_view_index() {
    return min(uint(gl_ViewIndex), 1u);
}

#ifndef T3D_INSTANCE_BRINGUP
struct InstanceMaterial {
    vec4 albedo;
    vec4 pbr;
    vec3 emission;
    uint flags;
    float normal_scale;
    uint reserved0;
    uvec2 albedo_texture_id;
    uvec4 normal_orm_ids;
};
layout(set = 0, binding = 3, std430) readonly buffer InstanceMaterials {
    InstanceMaterial materials[];
};
layout(set = 1, binding = 0) uniform sampler2D base_albedo_texture;
layout(set = 1, binding = 1) uniform sampler2D base_normal_texture;
layout(set = 1, binding = 2) uniform sampler2D base_orm_texture;
layout(set = 1, binding = 3) uniform sampler2D overlay_albedo_texture;
layout(set = 1, binding = 4) uniform sampler2D overlay_normal_texture;
layout(set = 1, binding = 5) uniform sampler2D overlay_orm_texture;
#endif

layout(push_constant, std430) uniform InstancePush {
    uint material_index;
    uint overlay_material_index;
    uint visible_base;
    uint flags;
    uint visibility_layers;
    uint reserved0;
    uint reserved1;
    uint reserved2;
}
push_data;

layout(location = 0) in vec3 vertex_normal;
layout(location = 1) in vec4 vertex_tangent;
layout(location = 2) in vec4 mesh_vertex_color;
layout(location = 3) in vec4 instance_color;
layout(location = 4) in vec2 vertex_uv;
layout(location = 5) in vec3 vertex_world_position;
#ifdef T3D_FORWARD_MOTION
layout(location = 6) in vec4 current_screen_position;
layout(location = 7) in vec4 previous_screen_position;
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

#ifdef T3D_INSTANCE_BRINGUP
void main() {
    out_color = vec4(1.0, 0.0, 1.0, 1.0);
}
#else
const uint MATERIAL_ALPHA_SCISSOR = 1u << 0u;
const uint MATERIAL_HAS_ALBEDO_TEXTURE = 1u << 2u;
const uint MATERIAL_HAS_NORMAL_TEXTURE = 1u << 3u;
const uint MATERIAL_HAS_ORM_TEXTURE = 1u << 4u;
const uint MATERIAL_EMISSIVE = 1u << 5u;
const uint MATERIAL_VERTEX_COLOR = 1u << 7u;
const uint MATERIAL_VERTEX_COLOR_SRGB = 1u << 8u;
const uint MATERIAL_NORMAL_MAPPING = 1u << 10u;
const uint MATERIAL_AO_ENABLED = 1u << 11u;
const uint MATERIAL_UNSHADED = 1u << 12u;
const uint MATERIAL_ROUGHNESS_TEXTURE = 1u << 19u;
const uint INSTANCE_FADE_ACTIVE = 1u << 24u;
const uint INSTANCE_FADE_SHIFT = 8u;
const uint INSTANCE_DEPTH_PREPASS_PASS = 1u << 31u;

vec3 srgb_to_linear(vec3 color) {
    bvec3 cutoff = lessThanEqual(color, vec3(0.04045));
    vec3 lower = color / 12.92;
    vec3 upper = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(upper, lower, cutoff);
}

float material_texture_channel(vec4 value, uint channel) {
    if (channel == 0u) {
        return value.r;
    }
    if (channel == 1u) {
        return value.g;
    }
    if (channel == 2u) {
        return value.b;
    }
    if (channel == 3u) {
        return value.a;
    }
    return dot(value.rgb, vec3(0.33333334));
}

vec3 decode_normal(sampler2D normal_texture, vec2 uv, float normal_scale) {
    vec2 xy = texture(normal_texture, uv).rg * 2.0 - 1.0;
    xy *= normal_scale;
    float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    return normalize(vec3(xy, z));
}

vec3 tangent_to_world(vec3 tangent_normal) {
    vec3 n = normalize(vertex_normal);
    vec3 t = normalize(vertex_tangent.xyz - n * dot(n, vertex_tangent.xyz));
    vec3 b = normalize(cross(n, t)) * vertex_tangent.w;
    return normalize(mat3(t, b, n) * tangent_normal);
}

void main() {
#ifndef T3D_FORWARD_DEPTH
    if (push_data.reserved0 != 0u) {
        out_color = vec4(1.0, 0.0, 1.0, 1.0);
#ifdef T3D_FORWARD_SPECULAR
        out_specular = vec4(0.0);
#endif
#ifdef T3D_FORWARD_MOTION
        out_motion = vec2(0.0);
#endif
        return;
    }
#endif
    vec4 mesh_color = mesh_vertex_color;
    vec4 base_color = instance_color;
    vec3 normal = normalize(vertex_normal);
    float metallic = 0.0;
    float roughness = 1.0;
    float ao = 1.0;
    vec3 emission = vec3(0.0);
    bool unshaded = false;
    bool base_survives = true;

    if (push_data.material_index != 0xffffu) {
        InstanceMaterial material = materials[push_data.material_index];
        if ((material.flags & MATERIAL_VERTEX_COLOR_SRGB) != 0u) {
            mesh_color.rgb = srgb_to_linear(mesh_color.rgb);
        }
        if ((material.flags & MATERIAL_VERTEX_COLOR) != 0u) {
            base_color *= mesh_color;
        }
        vec4 texel = (material.flags & MATERIAL_HAS_ALBEDO_TEXTURE) != 0u ? texture(base_albedo_texture, vertex_uv) : vec4(1.0);
        if ((material.flags & MATERIAL_HAS_ALBEDO_TEXTURE) != 0u) {
            texel.rgb = srgb_to_linear(texel.rgb);
        }
        base_color *= material.albedo * texel;
#if !defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS)
        vec4 orm_texel = (material.flags & MATERIAL_HAS_ORM_TEXTURE) != 0u ? texture(base_orm_texture, vertex_uv) : vec4(1.0);
        bool roughness_only = (material.flags & MATERIAL_ROUGHNESS_TEXTURE) != 0u;
        float metallic_sample = roughness_only ? 1.0 : orm_texel.b;
        float roughness_sample = roughness_only ? material_texture_channel(orm_texel, material.reserved0 & 7u) : orm_texel.g;
        float ao_sample = roughness_only ? 1.0 : orm_texel.r;
        metallic = clamp(material.pbr.x * metallic_sample, 0.0, 1.0);
        roughness = clamp(material.pbr.y * roughness_sample, 0.02, 1.0);
        ao = (material.flags & MATERIAL_AO_ENABLED) != 0u ? mix(1.0, ao_sample, clamp(material.pbr.z, 0.0, 1.0)) : 1.0;
        if ((material.flags & MATERIAL_NORMAL_MAPPING) != 0u && (material.flags & MATERIAL_HAS_NORMAL_TEXTURE) != 0u) {
            normal = tangent_to_world(decode_normal(base_normal_texture, vertex_uv, material.normal_scale));
        }
        if ((material.flags & MATERIAL_EMISSIVE) != 0u) {
            emission += material.emission;
        }
#endif
        if ((material.flags & MATERIAL_ALPHA_SCISSOR) != 0u && base_color.a < material.pbr.w) {
            base_survives = false;
        }
#if !defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS)
        unshaded = (material.flags & MATERIAL_UNSHADED) != 0u;
#endif
    }

    vec4 final_color = base_survives ? base_color : vec4(0.0);
    bool overlay_survives = false;
    if (push_data.overlay_material_index != 0xffffu) {
        InstanceMaterial overlay = materials[push_data.overlay_material_index];
        vec4 overlay_mesh_color = mesh_vertex_color;
        if ((overlay.flags & MATERIAL_VERTEX_COLOR_SRGB) != 0u) {
            overlay_mesh_color.rgb = srgb_to_linear(overlay_mesh_color.rgb);
        }
        vec4 overlay_color = instance_color;
        if ((overlay.flags & MATERIAL_VERTEX_COLOR) != 0u) {
            overlay_color *= overlay_mesh_color;
        }
        vec4 overlay_texel = (overlay.flags & MATERIAL_HAS_ALBEDO_TEXTURE) != 0u ? texture(overlay_albedo_texture, vertex_uv) : vec4(1.0);
        if ((overlay.flags & MATERIAL_HAS_ALBEDO_TEXTURE) != 0u) {
            overlay_texel.rgb = srgb_to_linear(overlay_texel.rgb);
        }
        overlay_color *= overlay.albedo * overlay_texel;
        overlay_survives = (overlay.flags & MATERIAL_ALPHA_SCISSOR) == 0u || overlay_color.a >= overlay.pbr.w;
        if (overlay_survives) {
#if !defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS)
            vec4 overlay_orm_texel = (overlay.flags & MATERIAL_HAS_ORM_TEXTURE) != 0u ? texture(overlay_orm_texture, vertex_uv) : vec4(1.0);
            bool overlay_roughness_only = (overlay.flags & MATERIAL_ROUGHNESS_TEXTURE) != 0u;
            float overlay_metallic_sample = overlay_roughness_only ? 1.0 : overlay_orm_texel.b;
            float overlay_roughness_sample =
                overlay_roughness_only ? material_texture_channel(overlay_orm_texel, overlay.reserved0 & 7u) : overlay_orm_texel.g;
            float overlay_ao_sample = overlay_roughness_only ? 1.0 : overlay_orm_texel.r;
            float overlay_weight = clamp(overlay_color.a, 0.0, 1.0);
            metallic = mix(metallic, clamp(overlay.pbr.x * overlay_metallic_sample, 0.0, 1.0), overlay_weight);
            roughness = mix(roughness, clamp(overlay.pbr.y * overlay_roughness_sample, 0.02, 1.0), overlay_weight);
            float overlay_ao =
                (overlay.flags & MATERIAL_AO_ENABLED) != 0u ? mix(1.0, overlay_ao_sample, clamp(overlay.pbr.z, 0.0, 1.0)) : 1.0;
            ao = mix(ao, overlay_ao, overlay_weight);
            if ((overlay.flags & MATERIAL_NORMAL_MAPPING) != 0u && (overlay.flags & MATERIAL_HAS_NORMAL_TEXTURE) != 0u) {
                vec3 overlay_normal = tangent_to_world(decode_normal(overlay_normal_texture, vertex_uv, overlay.normal_scale));
                normal = normalize(mix(normal, overlay_normal, overlay_weight));
            }
            if ((overlay.flags & MATERIAL_EMISSIVE) != 0u) {
                emission += overlay.emission * overlay_weight;
            }
#else
            float overlay_weight = clamp(overlay_color.a, 0.0, 1.0);
#endif
            final_color.rgb = mix(final_color.rgb, overlay_color.rgb, overlay_weight);
            final_color.a = max(final_color.a, overlay_color.a);
#if !defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS)
            unshaded = unshaded || (overlay.flags & MATERIAL_UNSHADED) != 0u;
#endif
        }
    }

    if (!base_survives && !overlay_survives) {
        discard;
    }
    if ((push_data.flags & INSTANCE_DEPTH_PREPASS_PASS) != 0u && final_color.a < 0.5) {
        discard;
    }
    if ((push_data.flags & INSTANCE_FADE_ACTIVE) != 0u) {
        float visibility_fade = float((push_data.flags >> INSTANCE_FADE_SHIFT) & 0xffffu) / 65535.0;
        vec2 pixel = floor(gl_FragCoord.xy);
        float dither = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
        if (dither > visibility_fade) {
            discard;
        }
    }

#ifdef T3D_FORWARD_DEPTH
#ifdef T3D_FORWARD_NORMAL_ROUGHNESS
    out_normal_roughness = vec4(normalize(mat3(frame_data.view_matrix) * normal) * 0.5 + 0.5, roughness * (127.0 / 255.0));
#ifdef T3D_FORWARD_VOXEL_GI
    out_voxel_gi = uvec2(0xffu, 0xffu);
#endif
#endif
#else

    vec3 lit;
    vec3 specular_light = vec3(0.0);
    if (unshaded) {
        lit = final_color.rgb + emission;
    } else {
#ifdef T3D_FORWARD_NATIVE_LIGHTING
        vec3 view_position = (frame_data.view_matrix * vec4(vertex_world_position, 1.0)).xyz;
        vec3 view_normal = normalize(mat3(frame_data.view_matrix) * normal);
        t3d_forward_apply_decals(
            view_position, push_data.visibility_layers, final_color.rgb, view_normal, roughness, metallic, ao, emission);
        emission *= t3d_forward_light_frame.environment_params0.y;
        normal = normalize(transpose(mat3(frame_data.view_matrix)) * view_normal);
        vec3 direct_diffuse;
        vec3 direct_specular;
        float material_specular = 1.0 - roughness;
        t3d_forward_direct_lighting(final_color.rgb,
            vertex_world_position,
            normalize(normal),
            frame_data.view_matrix,
            frame_data.camera_position[t3d_view_index()].xyz,
            roughness,
            metallic,
            material_specular,
            push_data.visibility_layers,
            2u,
            true,
            0u,
            1.0,
            direct_diffuse,
            direct_specular);
        vec3 reflection_view_direction = normalize(-view_position);
        vec3 ambient_diffuse;
        vec3 environment_specular;
        t3d_forward_environment_lighting(final_color.rgb,
            view_normal,
            reflection_view_direction,
            roughness,
            metallic,
            ao,
            material_specular,
            ambient_diffuse,
            environment_specular);
        specular_light = direct_specular + environment_specular
            + t3d_forward_reflection(
                view_position, view_normal, reflection_view_direction, roughness, metallic, final_color.rgb, push_data.visibility_layers);
        lit = ambient_diffuse + direct_diffuse + emission;
#ifndef T3D_FORWARD_SPECULAR
        lit += specular_light;
#endif
#else
        float ambient = (0.22 + 0.08 * roughness) * ao;
        vec3 debug_light_direction = normalize(vec3(0.34, 0.83, 0.44));
        float ndotl = max(dot(normalize(normal), debug_light_direction), 0.0);
        lit = final_color.rgb * (ambient + (1.0 - metallic * 0.65) * ndotl * (1.0 - mix(0.10, 0.30, roughness))) + emission;
#endif
    }
    out_color = vec4(lit, final_color.a);
#ifdef T3D_FORWARD_SPECULAR
    out_specular = unshaded ? vec4(0.0) : vec4(specular_light, metallic);
#endif
#ifdef T3D_FORWARD_MOTION
    vec2 current_clip_xy = current_screen_position.xy / current_screen_position.w - frame_data.motion_jitter.xy;
    vec2 previous_clip_xy = previous_screen_position.xy / previous_screen_position.w - frame_data.motion_jitter.zw;
    out_motion = (previous_clip_xy - current_clip_xy) * 0.5;
    if (frame_data.motion_params.y > 0.5) {
        out_color.rgb = clamp(vec3(0.5 + out_motion * 8.0, length(out_motion) * 16.0), vec3(0.0), vec3(1.0));
    }
#endif
#endif // T3D_FORWARD_DEPTH
}
#endif // T3D_INSTANCE_BRINGUP
