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
feedback_color_native = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING";
feedback_color_specular = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
feedback_color_motion = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
feedback_color_specular_motion = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
feedback_color_native_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_specular_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_motion_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_specular_motion_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
depth = "#define T3D_FORWARD_DEPTH";
depth_normal_roughness = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS";
depth_normal_roughness_voxel_gi = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI";
color_diag_constant = "#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_diag_opacity = "#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_native_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_native_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_specular_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_specular_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_motion_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_motion_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_specular_motion_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_specular_motion_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_native_radiance_array_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_native_radiance_array_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_specular_radiance_array_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_specular_radiance_array_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_motion_radiance_array_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_motion_radiance_array_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
color_specular_motion_radiance_array_diag_constant = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
color_specular_motion_radiance_array_diag_opacity = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_native_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_native_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_specular_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_specular_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_motion_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_motion_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_specular_motion_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_specular_motion_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_native_radiance_array_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_native_radiance_array_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_specular_radiance_array_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_specular_radiance_array_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_motion_radiance_array_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_motion_radiance_array_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
feedback_color_specular_motion_radiance_array_diag_constant = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
feedback_color_specular_motion_radiance_array_diag_opacity = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
depth_diag_constant = "#define T3D_FORWARD_DEPTH\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
depth_diag_opacity = "#define T3D_FORWARD_DEPTH\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
depth_normal_roughness_diag_constant = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
depth_normal_roughness_diag_opacity = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";
depth_normal_roughness_voxel_gi_diag_constant = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI\n#define T3D_INSTANCE_GPU_DIAG_CONSTANT";
depth_normal_roughness_voxel_gi_diag_opacity = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI\n#define T3D_INSTANCE_GPU_DIAG_OPACITY";

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
    vec4 nomad_sun_shape;
    vec4 nomad_sun_diffuse;
    vec4 nomad_sun_specular;
    vec4 nomad_sun_rim;
    vec4 instance_texture_mip_bias; // xyz: category mip bias; w: packed filter overrides
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
struct InstanceDrawMeta {
    uint material_index;
    uint overlay_material_index;
    uint first_index;
    uint index_count;
    int vertex_offset;
    uint flags;
    uint visible_base;
    uint reserved;
};
layout(set = 0, binding = 3, std430) readonly buffer DrawMetadata {
    InstanceDrawMeta draw_metadata[];
};
layout(set = 0, binding = 4, std430) readonly buffer PreviousInstanceRecords {
    InstanceRecord previous_records[];
};
layout(set = 0, binding = 5, std430) readonly buffer VisibleDrawSlots {
    uint visible_draw_slots[];
};
layout(set = 0, binding = 6, std430) readonly buffer VisibleLayers {
    uint visible_layers[];
};
layout(set = 0, binding = 7, std430) readonly buffer VisibleFlags {
    uint visible_flags[];
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
#ifndef T3D_INSTANCE_BRINGUP
layout(location = 8) flat out uint draw_material_index;
layout(location = 9) flat out uint draw_overlay_material_index;
layout(location = 10) flat out uint draw_flags;
layout(location = 11) flat out uint draw_visibility_layers;
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
    uint visible_address = push_data.visible_base + uint(gl_InstanceIndex);
    uint record_index = visible_indices[visible_address];
#ifndef T3D_INSTANCE_BRINGUP
    draw_material_index = push_data.material_index;
    draw_overlay_material_index = push_data.overlay_material_index;
    draw_flags = push_data.flags;
    draw_visibility_layers = push_data.visibility_layers;
    if (push_data.reserved0 != 0u) {
        uint draw_slot = visible_draw_slots[visible_address];
        InstanceDrawMeta metadata = draw_metadata[draw_slot];
        draw_material_index = metadata.material_index;
        draw_overlay_material_index = metadata.overlay_material_index;
        draw_flags = metadata.flags | visible_flags[visible_address];
        draw_visibility_layers = visible_layers[visible_address];
    }
#endif
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
#if defined(T3D_FORWARD_NATIVE_LIGHTING) && !defined(T3D_FORWARD_DEPTH)
#extension GL_EXT_samplerless_texture_functions : enable
#endif
// Color variants opt into the shared set-3 feedback owner when it is available.
#ifdef T3D_VT_FRAGMENT_FEEDBACK
#define T3D_INSTANCE_FRAGMENT_FEEDBACK_PRODUCTION_DISABLED 0
#else
#define T3D_INSTANCE_FRAGMENT_FEEDBACK_PRODUCTION_DISABLED 1
#endif
#ifndef T3D_INSTANCE_BRINGUP
#include "vt/terrain_vt_common_inc.glslinc"
#include "vt/terrain_vt_filter_math_inc.glslinc"
#include "vt/terrain_vt_tail_filter_inc.glslinc"
#ifdef T3D_VT_FRAGMENT_FEEDBACK
// P22 fragment requests use their descriptor-invariant set 3 while P28/P33
// sampling stays on the canonical shared VT set 1. Rename only the include's
// block instances so both allocation/page-table views can coexist in this
// shader without changing the common feedback helper ABI.
#define vt_allocations vt_fragment_allocations
#define vt_page_table vt_fragment_page_table
#define T3D_VT_DECLARE_FRAGMENT_BINDINGS
#include "vt/terrain_vt_feedback_inc.glslinc"
#undef T3D_VT_DECLARE_FRAGMENT_BINDINGS
#undef vt_page_table
#undef vt_allocations
#endif
#include "terrain_forward_lighting_inc.glslinc"
#endif

layout(set = 0, binding = 0, std140) uniform InstanceFrameFragment {
    mat4 view_projection[2];
    mat4 view_matrix;
    mat4 previous_view_projection[2];
    vec4 camera_position[2];
    vec4 motion_jitter;
    vec4 motion_params;
    vec4 nomad_sun_shape;
    vec4 nomad_sun_diffuse;
    vec4 nomad_sun_specular;
    vec4 nomad_sun_rim;
    vec4 instance_texture_mip_bias; // xyz: category mip bias; w: packed filter overrides
}
frame_data;

uint t3d_view_index() {
    return min(uint(gl_ViewIndex), 1u);
}

#ifndef T3D_INSTANCE_BRINGUP
layout(set = 1, binding = 0, std430) readonly buffer InstanceMaterials {
    T3DRenderingMaterialGpu materials[];
};
layout(set = 1, binding = 1, std430) readonly buffer VtAllocations {
    T3DVTAllocation vt_allocations[];
};
layout(set = 1, binding = 2, std430) readonly buffer VtPageTable {
    T3DVTPageEntry vt_page_table[];
};
layout(set = 1, binding = 3) uniform sampler2DArray vt_albedo_bank0;
layout(set = 1, binding = 4) uniform sampler2DArray vt_normal_bank0;
layout(set = 1, binding = 5) uniform sampler2DArray vt_surface_bank0;
layout(set = 1, binding = 6, std430) readonly buffer VtTailMetadata {
    T3DVTTailMetadata vt_tails[];
};
layout(set = 1, binding = 7) uniform sampler2D vt_albedo_tail;
layout(set = 1, binding = 8) uniform sampler2D vt_normal_tail;
layout(set = 1, binding = 9) uniform sampler2D vt_surface_tail;
layout(set = 1, binding = 10, std430) readonly buffer VtMipLayoutBuffer {
    T3DVTMipLayout vt_mip_layouts[];
};
#define T3D_VT_RESOLVE_ALLOCATION(index) vt_allocations[index]
#define T3D_VT_RESOLVE_PAGE_ENTRY(index) vt_page_table[index]
#define T3D_VT_RESOLVE_TAIL_ENTRY(index) vt_tails[index]
#include "vt/terrain_vt_resolve_core.glslinc"
#undef T3D_VT_RESOLVE_TAIL_ENTRY
#undef T3D_VT_RESOLVE_PAGE_ENTRY
#undef T3D_VT_RESOLVE_ALLOCATION
#define T3D_VT_CONSUMER_ALLOCATION(index) vt_allocations[index]
#define T3D_VT_CONSUMER_MIP_LAYOUT(index) vt_mip_layouts[index]
#define T3D_VT_CONSUMER_TAIL(index) vt_tails[index]
#define T3D_VT_CONSUMER_ALBEDO_BANK_GRAD(layer, uv, grad_x, grad_y) textureGrad(vt_albedo_bank0, vec3((uv), float(layer)), (grad_x), (grad_y))
#define T3D_VT_CONSUMER_NORMAL_BANK_GRAD(layer, uv, grad_x, grad_y) textureGrad(vt_normal_bank0, vec3((uv), float(layer)), (grad_x), (grad_y))
#define T3D_VT_CONSUMER_SURFACE_BANK_GRAD(layer, uv, grad_x, grad_y) textureGrad(vt_surface_bank0, vec3((uv), float(layer)), (grad_x), (grad_y))
#define T3D_VT_CONSUMER_ALBEDO_TAIL_LOD0(uv) textureLod(vt_albedo_tail, (uv), 0.0)
#define T3D_VT_CONSUMER_NORMAL_TAIL_LOD0(uv) textureLod(vt_normal_tail, (uv), 0.0)
#define T3D_VT_CONSUMER_SURFACE_TAIL_LOD0(uv) textureLod(vt_surface_tail, (uv), 0.0)
#define T3D_VT_CONSUMER_ALBEDO_TAIL_SIZE() textureSize(vt_albedo_tail, 0)
#define T3D_VT_CONSUMER_NORMAL_TAIL_SIZE() textureSize(vt_normal_tail, 0)
#define T3D_VT_CONSUMER_SURFACE_TAIL_SIZE() textureSize(vt_surface_tail, 0)
#include "vt/terrain_vt_consumer_sample_inc.glslinc"
#undef T3D_VT_CONSUMER_SURFACE_TAIL_SIZE
#undef T3D_VT_CONSUMER_NORMAL_TAIL_SIZE
#undef T3D_VT_CONSUMER_ALBEDO_TAIL_SIZE
#undef T3D_VT_CONSUMER_SURFACE_TAIL_LOD0
#undef T3D_VT_CONSUMER_NORMAL_TAIL_LOD0
#undef T3D_VT_CONSUMER_ALBEDO_TAIL_LOD0
#undef T3D_VT_CONSUMER_SURFACE_BANK_GRAD
#undef T3D_VT_CONSUMER_NORMAL_BANK_GRAD
#undef T3D_VT_CONSUMER_ALBEDO_BANK_GRAD
#undef T3D_VT_CONSUMER_TAIL
#undef T3D_VT_CONSUMER_MIP_LAYOUT
#undef T3D_VT_CONSUMER_ALLOCATION
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
#ifndef T3D_INSTANCE_BRINGUP
layout(location = 8) flat in uint draw_material_index;
layout(location = 9) flat in uint draw_overlay_material_index;
layout(location = 10) flat in uint draw_flags;
layout(location = 11) flat in uint draw_visibility_layers;
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
    if (channel == 0u)
        return value.r;
    if (channel == 1u)
        return value.g;
    if (channel == 2u)
        return value.b;
    return value.a;
}

float t3d_inst_unpack_unorm8(uint packed, uint shift) {
    return float((packed >> shift) & T3D_VT_MATERIAL_UNORM8_MASK) * (1.0 / 255.0);
}

bool t3d_inst_sampler_repeats(uint sampler_class) {
    return sampler_class == T3D_VT_SAMPLER_LINEAR_REPEAT || sampler_class == T3D_VT_SAMPLER_NEAREST_REPEAT;
}

bool t3d_inst_sampler_nearest(uint sampler_class) {
    return sampler_class == T3D_VT_SAMPLER_NEAREST_REPEAT || sampler_class == T3D_VT_SAMPLER_NEAREST_CLAMP;
}

uint t3d_inst_filter_mode(T3DRenderingMaterialGpu material) {
    return t3d_vt_instance_filter_mode(
        (material.packedScalars1 & T3D_VT_MATERIAL_FILTER_MASK) >> T3D_VT_MATERIAL_FILTER_SHIFT,
        frame_data.instance_texture_mip_bias.w, (draw_flags >> 26u) & 3u);
}

uint t3d_inst_effective_sampler_class(T3DRenderingMaterialGpu material) {
    bool repeat_uv = material.samplerClass == T3D_VT_SAMPLER_LINEAR_REPEAT || material.samplerClass == T3D_VT_SAMPLER_NEAREST_REPEAT;
    uint filterMode = t3d_inst_filter_mode(material);
    bool nearest = filterMode == T3D_VT_FILTER_NEAREST || filterMode == T3D_VT_FILTER_NEAREST_MIPMAP
        || filterMode == T3D_VT_FILTER_NEAREST_MIPMAP_ANISOTROPIC;
    return nearest ? (repeat_uv ? T3D_VT_SAMPLER_NEAREST_REPEAT : T3D_VT_SAMPLER_NEAREST_CLAMP)
                   : (repeat_uv ? T3D_VT_SAMPLER_LINEAR_REPEAT : T3D_VT_SAMPLER_LINEAR_CLAMP);
}

vec3 t3d_inst_unpack_rgbe9995(uint packed) {
    uvec3 mantissa = uvec3(packed & 0x1ffu, (packed >> 9u) & 0x1ffu, (packed >> 18u) & 0x1ffu);
    uint exponent = (packed >> 27u) & 0x1fu;
    return vec3(mantissa) * exp2(float(exponent) - 24.0);
}

T3DVTConsumerFootprint t3d_inst_material_footprint(T3DRenderingMaterialGpu material) {
    vec2 uv = vertex_uv * material.uvScaleBias.xy + material.uvScaleBias.zw;
    return t3d_vt_consumer_prepare_footprint(uv, dFdx(uv), dFdy(uv),
        frame_data.instance_texture_mip_bias[min((draw_flags >> 26u) & 3u, 2u)],
        t3d_inst_sampler_repeats(t3d_inst_effective_sampler_class(material)),
        t3d_inst_filter_mode(material), push_data.reserved1, push_data.reserved2);
}

#ifdef T3D_VT_FRAGMENT_FEEDBACK
void t3d_vt_instance_fragment_feedback_channel(T3DRenderingMaterialGpu material,
    T3DVTConsumerFootprint footprint, uint channel) {
    if (!t3d_vt_consumer_material_page_set_valid(material.vtAllocation, footprint)) return;
    t3d_vt_fragment_request_material_page_set(material.vtAllocation + channel,
        footprint.wrapped_uv, footprint.ddx_uv, footprint.ddy_uv, footprint.mip_bias,
        T3D_VT_FRAGMENT_PRIORITY_COLOR, footprint.repeat_uv != 0u, footprint.filter_mode);
}
#endif

vec3 decode_normal_sample(vec4 normal_texel, float normal_scale) {
    vec2 xy = (normal_texel.rg * 2.0 - 1.0) * normal_scale;
    float len_sq = dot(xy, xy);
    if (len_sq > 1.0) {
        xy *= inversesqrt(len_sq);
        len_sq = 1.0;
    }
    return vec3(xy, sqrt(max(1.0 - len_sq, 0.0)));
}

vec3 face_oriented_normal(vec3 normal_value) {
    vec3 n = normalize(normal_value);
    return gl_FrontFacing ? n : -n;
}

vec3 tangent_to_world(vec3 tangent_normal, vec3 face_normal) {
    vec3 base_n = gl_FrontFacing ? face_normal : -face_normal;
    vec3 t = normalize(vertex_tangent.xyz - face_normal * dot(face_normal, vertex_tangent.xyz));
    float handedness = gl_FrontFacing ? vertex_tangent.w : -vertex_tangent.w;
    vec3 b = normalize(cross(base_n, t)) * handedness;
    return normalize(mat3(t, b, base_n) * tangent_normal);
}

void t3d_inst_evaluate_surface(T3DRenderingMaterialGpu material,
    T3DVTConsumerFootprint footprint,
    out T3DVTConsumerAddress vt_address,
    out float metallic,
    out float roughness,
    out float ao,
    out float normal_scale) {
    bool has_surface = t3d_vt_material_has_surface_texture(material.flags);
    vec4 surface_texel = vec4(1.0);
    if (has_surface) {
        T3DVTConsumerAddress surface_address = t3d_vt_consumer_resolve_material_channel_v3(material.vtAllocation, 2u, footprint);
        surface_texel = t3d_vt_consumer_sample_orm_v3(surface_address);
#ifdef T3D_VT_FRAGMENT_FEEDBACK
        t3d_vt_instance_fragment_feedback_channel(material, footprint, 2u);
#endif
    }
    vt_address = t3d_vt_consumer_invalid_address();
    if ((material.flags & T3D_VT_MATERIAL_NORMAL_MAPPING) != 0u) {
        vt_address = t3d_vt_consumer_resolve_material_channel_v3(material.vtAllocation, 1u, footprint);
    }
#ifdef T3D_VT_FRAGMENT_FEEDBACK
    if ((material.flags & T3D_VT_MATERIAL_NORMAL_MAPPING) != 0u) {
        t3d_vt_instance_fragment_feedback_channel(material, footprint, 1u);
    }
#endif
    metallic = t3d_inst_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_METALLIC_SHIFT);
    roughness = t3d_inst_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_ROUGHNESS_SHIFT);
    float ao_strength = t3d_inst_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_AO_SHIFT);
    normal_scale = unpackHalf2x16(material.packedScalars1).x;
    uint surface_layout = (material.packedScalars1 & T3D_VT_MATERIAL_SURFACE_LAYOUT_MASK) >> T3D_VT_MATERIAL_SURFACE_LAYOUT_SHIFT;
    if (!has_surface) {
        ao = 1.0;
        roughness = clamp(roughness, 0.02, 1.0);
        metallic = clamp(metallic, 0.0, 1.0);
    } else if (surface_layout == 0u) {
        ao = (material.flags & T3D_VT_MATERIAL_AO_ENABLED) != 0u ? mix(1.0, surface_texel.r, ao_strength) : 1.0;
        if ((material.flags & T3D_VT_MATERIAL_DIRECT_ORM) != 0u) {
            roughness = clamp(surface_texel.g, 0.04, 1.0);
            metallic = clamp(surface_texel.b, 0.0, 1.0);
        } else {
            roughness = clamp(roughness * surface_texel.g, 0.02, 1.0);
            metallic = clamp(metallic * surface_texel.b, 0.0, 1.0);
        }
    } else {
        ao = 1.0;
        roughness = clamp(roughness * material_texture_channel(surface_texel, min(surface_layout - 1u, 3u)), 0.02, 1.0);
        metallic = clamp(metallic, 0.0, 1.0);
    }
}

float t3d_inst_blinn_lobe(float ndoth, float roughness, float width_value, float roughness_influence) {
    float x2 = ndoth * ndoth;
    float x4 = x2 * x2;
    float x8 = x4 * x4;
    float x16 = x8 * x8;
    float x32 = x16 * x16;
    float smoothness = 1.0 - roughness;
    float roughness_shape = mix(1.0, smoothness * smoothness, roughness_influence);
    float width = clamp(width_value * roughness_shape, 0.0, 1.0);
    float broad = mix(x4, x8, width);
    float narrow = mix(x16, x32, width);
    float lobe = mix(broad, narrow, width);
    return lobe * mix(0.08, 1.0, width);
}

#ifdef T3D_FORWARD_NATIVE_LIGHTING
void t3d_inst_stylized_local_light_response(vec3 albedo,
    vec3 N,
    vec3 V,
    float roughness,
    float metallic,
    float dielectric_specular,
    vec3 L,
    vec3 radiance,
    float attenuation,
    float specular_amount,
    inout vec3 diffuse_light,
    inout vec3 specular_light) {
    float ndotl = dot(N, L);
    float lambert = max(ndotl, 0.0);
    if (lambert <= 0.0 || attenuation <= 0.0) return;
    vec3 incoming_light = radiance * attenuation / T3D_PI;
    float diffuse_metal_factor = 1.0 - metallic * 0.88;
    diffuse_light += albedo * lambert * diffuse_metal_factor * incoming_light;
    vec3 H = normalize(L + V);
    float ndoth = max(dot(N, H), 0.0);
    float x2 = ndoth * ndoth;
    float x4 = x2 * x2;
    float x8 = x4 * x4;
    float x16 = x8 * x8;
    float smoothness = 1.0 - roughness;
    float local_specular = mix(x8, x16, smoothness) * smoothness * smoothness;
    vec3 specular_color = mix(vec3(dielectric_specular), albedo, metallic);
    specular_light += specular_color * local_specular * 0.12 * max(specular_amount, 0.0) * incoming_light;
}

void t3d_inst_stylized_direct_response(vec3 albedo,
    T3DForwardShadingContext context,
    float roughness,
    float metallic,
    float dielectric_specular,
    uint surface_mask,
    out vec3 diffuse_light,
    out vec3 specular_light) {
    diffuse_light = vec3(0.0);
    specular_light = vec3(0.0);
    vec3 N = normalize(context.view_normal);
    vec3 V = normalize(context.view_direction);
    float diffuse_metal_factor = 1.0 - metallic * 0.88;

    uint directional_count = min(t3d_forward_light_frame.scene_params.x, 8u);
    for (uint i = 0u; i < directional_count; ++i) {
        T3DNativeDirectionalLightData light = t3d_directional_lights.data[i];
        if ((light.mask & surface_mask) == 0u) continue;
        vec3 L = normalize(light.direction);
        float ndotl = dot(N, L);
        float lambert = max(ndotl, 0.0);
        float shadow = t3d_directional_shadow(i, context.view_position, N);
        vec3 incoming_light = light.color * light.energy * shadow / T3D_PI;
        float wrapped = smoothstep(-frame_data.nomad_sun_shape.x, max(frame_data.nomad_sun_shape.y, 0.001), ndotl);
        float diffuse_term = mix(lambert, wrapped, frame_data.nomad_sun_shape.z);
        float contrast_scale = 1.0 + frame_data.nomad_sun_shape.w * 3.5;
        diffuse_term = clamp((diffuse_term - 0.5) * contrast_scale + 0.5, 0.0, 1.0);
        diffuse_term = max(diffuse_term, frame_data.nomad_sun_diffuse.z * (1.0 - lambert));
        float face2 = diffuse_term * diffuse_term;
        float face4 = face2 * face2;
        diffuse_term = min(diffuse_term * (1.0 + face4 * frame_data.nomad_sun_diffuse.y), 2.0);
        diffuse_light += albedo * diffuse_term * diffuse_metal_factor * frame_data.nomad_sun_diffuse.x * incoming_light;
        if (ndotl > 0.0) {
            vec3 H = normalize(L + V);
            float ndoth = max(dot(N, H), 0.0);
            float specular_lobe = t3d_inst_blinn_lobe(ndoth, roughness, frame_data.nomad_sun_specular.y, frame_data.nomad_sun_specular.z);
            vec3 specular_color = mix(vec3(dielectric_specular), albedo, metallic);
            specular_light += specular_color * specular_lobe * frame_data.nomad_sun_specular.x * max(light.specular, 0.0) * incoming_light;
        }
        float nv = max(dot(N, V), 0.0);
        float rim = 1.0 - nv;
        float rim2 = rim * rim;
        float rim4 = rim2 * rim2;
        float rim8 = rim4 * rim4;
        float rim_shape = mix(rim2, rim8, frame_data.nomad_sun_rim.y);
        float backlight = max(dot(-N, L), 0.0);
        diffuse_light += albedo * rim_shape * backlight * frame_data.nomad_sun_rim.x * incoming_light;
    }

    for (uint light_type = 0u; light_type < 3u; ++light_type) {
        uint type_offset, item_min, item_max, item_from, item_to;
        if (!t3d_cluster_range(context.cluster_address, light_type, type_offset, item_min, item_max, item_from, item_to)) continue;
        for (uint word = item_from; word < item_to; ++word) {
            uint mask = t3d_cluster_buffer.data[type_offset + word] & t3d_cluster_range_mask(word, item_min, item_max);
            while (mask != 0u) {
                uint bit = uint(findMSB(mask));
                mask &= ~(1u << bit);
                uint light_index = word * 32u + bit;
                T3DNativeLightData light = light_type == 0u
                    ? t3d_omni_lights.data[light_index]
                    : (light_type == 1u ? t3d_spot_lights.data[light_index] : t3d_area_lights.data[light_index]);
                if ((light.mask & surface_mask) == 0u) continue;
                vec3 light_direction;
                float distance_to_light;
                float attenuation;
                float shadow = 1.0;
                if (light_type == 0u || light_type == 1u) {
                    vec3 to_light = light.position - context.view_position;
                    distance_to_light = length(to_light);
                    if (distance_to_light <= 1e-6) continue;
                    light_direction = to_light / distance_to_light;
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                }
                if (light_type == 0u) {
                    shadow = t3d_omni_shadow(light, context.view_position, N);
                } else if (light_type == 1u) {
                    float cone_angle = light.cone_angle;
                    float scos = max(dot(-light_direction, light.direction), cone_angle);
                    float spot_rim = max(1e-4, (1.0 - scos) / max(1.0 - cone_angle, 1e-4));
                    attenuation *= 1.0 - pow(spot_rim, light.cone_attenuation);
                    shadow = t3d_spot_shadow(light, context.view_position, N, light_direction, distance_to_light);
                } else {
                    if (dot(light.direction, context.view_position - light.position) <= 0.0) continue;
                    float width_length = length(light.area_width);
                    float height_length = length(light.area_height);
                    if (width_length <= 1e-6 || height_length <= 1e-6) continue;
                    vec3 width_direction = light.area_width / width_length;
                    vec3 height_direction = light.area_height / height_length;
                    vec3 from_center = context.view_position - light.position;
                    float local_x = clamp(dot(from_center, width_direction), -0.5 * width_length, 0.5 * width_length);
                    float local_y = clamp(dot(from_center, height_direction), -0.5 * height_length, 0.5 * height_length);
                    vec3 closest_point = light.position + width_direction * local_x + height_direction * local_y;
                    vec3 to_light = closest_point - context.view_position;
                    distance_to_light = length(to_light);
                    if (distance_to_light <= 1e-6) continue;
                    light_direction = to_light / distance_to_light;
                    attenuation = t3d_native_attenuation(distance_to_light, light.inv_radius, light.attenuation);
                    float area = max(width_length * height_length, 0.0001);
                    attenuation *= clamp(area / max(area, distance_to_light * distance_to_light), 0.0, 1.0);
                    shadow = t3d_area_shadow(light, context.view_position, N);
                }
                t3d_inst_stylized_local_light_response(albedo, N, V, roughness, metallic, dielectric_specular,
                    light_direction, light.color, attenuation * shadow, light.specular_amount, diffuse_light, specular_light);
            }
        }
    }
}
#endif

void main() {
    vec4 mesh_color = mesh_vertex_color;
    vec4 base_color = instance_color;
    vec4 overlay_color = vec4(0.0);
    vec3 normal = face_oriented_normal(vertex_normal);
    float metallic = 0.0;
    float roughness = 1.0;
    float ao = 1.0;
    vec3 emission = vec3(0.0);
    bool unshaded = false;
    bool stylized_sun = false;
    float dielectric_specular = 0.04;
    bool base_survives = true;
    bool overlay_survives = false;
    T3DRenderingMaterialGpu material;
    T3DRenderingMaterialGpu overlay;
    T3DVTConsumerFootprint base_footprint;
    T3DVTConsumerFootprint overlay_footprint;

#ifndef T3D_INSTANCE_GPU_DIAG_CONSTANT
    // Capture both materials' derivatives before fade/scissor divergence.
    if (draw_material_index != 0xffffu) {
        material = materials[draw_material_index];
        base_footprint = t3d_inst_material_footprint(material);
    }
    if (draw_overlay_material_index != 0xffffu) {
        overlay = materials[draw_overlay_material_index];
        overlay_footprint = t3d_inst_material_footprint(overlay);
    }
#endif
    if ((draw_flags & INSTANCE_FADE_ACTIVE) != 0u) {
        float visibility_fade = float((draw_flags >> INSTANCE_FADE_SHIFT) & 0xffffu) / 65535.0;
        vec2 pixel = floor(gl_FragCoord.xy);
        float dither = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
        if (dither > visibility_fade) discard;
    }

#ifndef T3D_INSTANCE_GPU_DIAG_CONSTANT
    // Resolve only opacity until the combined base/overlay pixel survives.
    if (draw_material_index != 0xffffu) {
        if ((material.flags & T3D_VT_MATERIAL_VERTEX_COLOR_SRGB) != 0u)
            mesh_color.rgb = srgb_to_linear(mesh_color.rgb);
        if ((material.flags & T3D_VT_MATERIAL_VERTEX_COLOR) != 0u)
            base_color *= mesh_color;
#ifdef T3D_VT_FRAGMENT_FEEDBACK
        t3d_vt_instance_fragment_feedback_channel(material, base_footprint, 0u);
#endif
        T3DVTConsumerAddress albedo_address = t3d_vt_consumer_resolve_material_channel_v3(material.vtAllocation, 0u, base_footprint);
        base_color *= material.baseColor * t3d_vt_consumer_sample_albedo(albedo_address);
        if ((material.flags & T3D_VT_MATERIAL_ALBEDO_SATURATION) != 0u) {
            float saturation = float(material.reserved & 0xffffu) * (1.5 / 65535.0);
            float luma = dot(base_color.rgb, vec3(0.299, 0.587, 0.114));
            base_color.rgb = max(mix(vec3(luma), base_color.rgb, saturation), vec3(0.0));
        }
        float alpha_cutoff = t3d_inst_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_ALPHA_SCISSOR_SHIFT);
        base_survives = (material.flags & T3D_VT_MATERIAL_ALPHA_SCISSOR) == 0u || base_color.a >= alpha_cutoff;
    }
    vec4 final_color = base_survives ? base_color : vec4(0.0);
    if (draw_overlay_material_index != 0xffffu) {
        vec4 overlay_mesh_color = mesh_vertex_color;
        if ((overlay.flags & T3D_VT_MATERIAL_VERTEX_COLOR_SRGB) != 0u)
            overlay_mesh_color.rgb = srgb_to_linear(overlay_mesh_color.rgb);
        overlay_color = instance_color;
        if ((overlay.flags & T3D_VT_MATERIAL_VERTEX_COLOR) != 0u)
            overlay_color *= overlay_mesh_color;
#ifdef T3D_VT_FRAGMENT_FEEDBACK
        t3d_vt_instance_fragment_feedback_channel(overlay, overlay_footprint, 0u);
#endif
        T3DVTConsumerAddress overlay_albedo_address = t3d_vt_consumer_resolve_material_channel_v3(overlay.vtAllocation, 0u, overlay_footprint);
        overlay_color *= overlay.baseColor * t3d_vt_consumer_sample_albedo(overlay_albedo_address);
        float overlay_alpha_cutoff = t3d_inst_unpack_unorm8(overlay.packedScalars0, T3D_VT_MATERIAL_ALPHA_SCISSOR_SHIFT);
        overlay_survives = (overlay.flags & T3D_VT_MATERIAL_ALPHA_SCISSOR) == 0u || overlay_color.a >= overlay_alpha_cutoff;
        if (overlay_survives) {
            final_color.rgb = mix(final_color.rgb, overlay_color.rgb, clamp(overlay_color.a, 0.0, 1.0));
            final_color.a = max(final_color.a, overlay_color.a);
        }
    }
    if (!base_survives && !overlay_survives) discard;
    if ((draw_flags & INSTANCE_DEPTH_PREPASS_PASS) != 0u && final_color.a < 0.5) discard;
#else
    vec4 final_color = vec4(0.5, 0.5, 0.5, 1.0);
#endif

#if (!defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS)) && !defined(T3D_INSTANCE_GPU_DIAG_CONSTANT) && !defined(T3D_INSTANCE_GPU_DIAG_OPACITY)
    if (draw_material_index != 0xffffu) {
        T3DVTConsumerAddress vt_address;
        float normal_scale;
        // Preserve the base's contribution under a partially opaque overlay,
        // including when the base opacity itself did not survive scissor.
        t3d_inst_evaluate_surface(material, base_footprint, vt_address, metallic, roughness, ao, normal_scale);
        if ((material.flags & T3D_VT_MATERIAL_NORMAL_MAPPING) != 0u) {
            normal = tangent_to_world(decode_normal_sample(t3d_vt_consumer_sample_normal_v3(vt_address), normal_scale), normal);
        }
        emission += t3d_inst_unpack_rgbe9995(material.packedEmission);
        unshaded = (material.flags & T3D_VT_MATERIAL_UNSHADED) != 0u;
        stylized_sun = (material.flags & T3D_VT_MATERIAL_STYLIZED_SUN) != 0u;
        dielectric_specular = float((material.reserved >> 16u) & 0xffffu) / 65535.0;
    }
    if (overlay_survives) {
        T3DVTConsumerAddress overlay_vt_address;
        float overlay_metallic, overlay_roughness, overlay_ao, overlay_normal_scale;
        t3d_inst_evaluate_surface(overlay, overlay_footprint, overlay_vt_address,
            overlay_metallic, overlay_roughness, overlay_ao, overlay_normal_scale);
        float overlay_weight = clamp(overlay_color.a, 0.0, 1.0);
        metallic = mix(metallic, overlay_metallic, overlay_weight);
        roughness = mix(roughness, overlay_roughness, overlay_weight);
        ao = mix(ao, overlay_ao, overlay_weight);
        if ((overlay.flags & T3D_VT_MATERIAL_NORMAL_MAPPING) != 0u) {
            vec3 overlay_normal = tangent_to_world(
                decode_normal_sample(t3d_vt_consumer_sample_normal_v3(overlay_vt_address), overlay_normal_scale), normal);
            normal = normalize(mix(normal, overlay_normal, overlay_weight));
        }
        emission += t3d_inst_unpack_rgbe9995(overlay.packedEmission) * overlay_weight;
        unshaded = unshaded || (overlay.flags & T3D_VT_MATERIAL_UNSHADED) != 0u;
    }
#endif

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
#if defined(T3D_INSTANCE_GPU_DIAG_CONSTANT) || defined(T3D_INSTANCE_GPU_DIAG_OPACITY)
    unshaded = true;
    lit = vec3(0.5);
#else
    if (unshaded) {
        lit = final_color.rgb + emission;
    } else {
#ifdef T3D_FORWARD_NATIVE_LIGHTING
        vec3 view_position = (frame_data.view_matrix * vec4(vertex_world_position, 1.0)).xyz;
        vec3 view_normal = normalize(mat3(frame_data.view_matrix) * normal);
        T3DForwardShadingContext shading_context = t3d_forward_make_shading_context(view_position, view_normal);
        t3d_forward_apply_decals(shading_context, draw_visibility_layers, final_color.rgb, roughness, metallic, ao, emission);
        emission *= t3d_forward_light_frame.environment_params0.y;
        vec3 direct_diffuse;
        vec3 direct_specular;
        float material_specular = stylized_sun ? 0.30 : (1.0 - roughness);
        if (stylized_sun) {
            t3d_inst_stylized_direct_response(final_color.rgb, shading_context, roughness, metallic, dielectric_specular,
                draw_visibility_layers, direct_diffuse, direct_specular);
        } else {
            t3d_forward_direct_lighting(final_color.rgb,
                shading_context,
                roughness,
                metallic,
                material_specular,
                draw_visibility_layers,
                2u,
                0u,
                1.0,
                direct_diffuse,
                direct_specular);
        }
        vec3 ambient_diffuse;
        vec3 environment_specular;
        t3d_forward_environment_lighting(
            final_color.rgb, shading_context, roughness, metallic, ao, material_specular, ambient_diffuse, environment_specular);
        specular_light = direct_specular + environment_specular
            + t3d_forward_reflection(shading_context, roughness, metallic, final_color.rgb, draw_visibility_layers);
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
#endif
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
