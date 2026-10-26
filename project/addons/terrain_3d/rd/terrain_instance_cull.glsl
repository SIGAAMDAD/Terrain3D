#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform InstanceFrame {
    mat4 view_projection[2];
    mat4 view_matrix;
    mat4 previous_view_projection[2];
    vec4 camera_position[2];
    vec4 motion_jitter;
    vec4 motion_params;
}
frame_data;

struct InstanceCullDescriptor {
    vec4 bounds_center;
    vec4 bounds_extent;
    uint draw_slot;
    uint command_index;
    uint first_instance;
    uint instance_count;
    uint first_visible;
    uint visibility_layers;
    uint flags;
    float lod_begin;
    float lod_end;
};
layout(set = 0, binding = 1, std430) readonly buffer DescriptorBuffer {
    InstanceCullDescriptor descriptors[];
};
layout(set = 0, binding = 2, std430) buffer MainVisibleBuffer {
    uint main_visible[];
};
struct IndexedIndirectCommand {
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};
layout(set = 0, binding = 3, std430) buffer MainIndirectBuffer {
    IndexedIndirectCommand main_commands[];
};
layout(set = 0, binding = 4, std430) buffer ShadowVisibleBuffer {
    uint shadow_visible[];
};
layout(set = 0, binding = 5, std430) buffer ShadowIndirectBuffer {
    IndexedIndirectCommand shadow_commands[];
};
layout(set = 0, binding = 6, std430) buffer MainVisibleDrawSlotBuffer {
    uint main_visible_draw_slots[];
};
layout(set = 0, binding = 7, std430) buffer ShadowVisibleDrawSlotBuffer {
    uint shadow_visible_draw_slots[];
};
layout(set = 0, binding = 8, std430) buffer MainVisibleLayerBuffer {
    uint main_visible_layers[];
};
layout(set = 0, binding = 9, std430) buffer ShadowVisibleLayerBuffer {
    uint shadow_visible_layers[];
};

layout(push_constant, std430) uniform CullData {
    uint descriptor_base;
    uint descriptor_count;
    uint command_count;
    uint visibility_mask;
    uint view_count;
    uint shadow_pass;
    uint mode;
    uint main_command_base;
    uint shadow_command_base;
    uint reserved_0;
    uint reserved_1;
    uint reserved_2;
}
cull_data;

const uint T3D_RD_INSTANCE_SHADOWS_ONLY = 1u << 2u;
const uint T3D_RD_INSTANCE_HLOD = 1u << 25u;
const uint T3D_RD_INSTANCE_LOD_PRESELECTED = 1u << 28u;

shared uint shared_visible;
shared uint shared_write_base;

bool aabb_visible_view(vec3 c, vec3 e, uint view_index) {
    vec4 corners[8];
    mat4 vp = frame_data.view_projection[min(view_index, 1u)];
    corners[0] = vp * vec4(c.x - e.x, c.y - e.y, c.z - e.z, 1.0);
    corners[1] = vp * vec4(c.x + e.x, c.y - e.y, c.z - e.z, 1.0);
    corners[2] = vp * vec4(c.x - e.x, c.y + e.y, c.z - e.z, 1.0);
    corners[3] = vp * vec4(c.x + e.x, c.y + e.y, c.z - e.z, 1.0);
    corners[4] = vp * vec4(c.x - e.x, c.y - e.y, c.z + e.z, 1.0);
    corners[5] = vp * vec4(c.x + e.x, c.y - e.y, c.z + e.z, 1.0);
    corners[6] = vp * vec4(c.x - e.x, c.y + e.y, c.z + e.z, 1.0);
    corners[7] = vp * vec4(c.x + e.x, c.y + e.y, c.z + e.z, 1.0);
    bool outside_left = true;
    bool outside_right = true;
    bool outside_bottom = true;
    bool outside_top = true;
    bool outside_near = true;
    bool outside_far = true;
    for (int i = 0; i < 8; ++i) {
        vec4 p = corners[i];
        outside_left = outside_left && p.x < -p.w;
        outside_right = outside_right && p.x > p.w;
        outside_bottom = outside_bottom && p.y < -p.w;
        outside_top = outside_top && p.y > p.w;
        outside_near = outside_near && p.z > p.w;
        outside_far = outside_far && p.z < 0.0;
    }
    return !(outside_left || outside_right || outside_bottom || outside_top || outside_near || outside_far);
}

bool bounds_visible_view(InstanceCullDescriptor descriptor, uint view_index) {
    return aabb_visible_view(descriptor.bounds_center.xyz, descriptor.bounds_extent.xyz, view_index);
}

bool descriptor_visible(InstanceCullDescriptor descriptor) {
    if (descriptor.instance_count == 0u || (descriptor.visibility_layers & cull_data.visibility_mask) == 0u)
        return false;
    if (cull_data.shadow_pass == 0u) {
        if ((descriptor.flags & T3D_RD_INSTANCE_SHADOWS_ONLY) != 0u)
            return false;
        if ((descriptor.flags & (T3D_RD_INSTANCE_HLOD | T3D_RD_INSTANCE_LOD_PRESELECTED)) == 0u) {
            vec3 delta = descriptor.bounds_center.xyz - frame_data.camera_position[0].xyz;
            float distance_sq = dot(delta, delta);
            if (descriptor.lod_begin > 0.001 && distance_sq < descriptor.lod_begin * descriptor.lod_begin)
                return false;
            if (descriptor.lod_end > 0.001 && distance_sq >= descriptor.lod_end * descriptor.lod_end)
                return false;
        }
    }
    uint views = clamp(cull_data.view_count, 1u, 2u);
    bool frustum_visible = false;
    for (uint view = 0u; view < views; ++view) {
        if (bounds_visible_view(descriptor, view)) {
            frustum_visible = true;
            break;
        }
    }
    if (!frustum_visible)
        return false;
    return true;
}

void main() {
    if (cull_data.mode == 0u) {
        uint command_index = gl_GlobalInvocationID.x;
        if (command_index < cull_data.command_count) {
            if (cull_data.shadow_pass != 0u)
                shadow_commands[cull_data.shadow_command_base + command_index].instance_count = 0u;
            else
                main_commands[cull_data.main_command_base + command_index].instance_count = 0u;
        }
        return;
    }

    uint local_index = gl_LocalInvocationID.x;
    uint descriptor_index = gl_WorkGroupID.x;
    if (descriptor_index >= cull_data.descriptor_count)
        return;
    InstanceCullDescriptor descriptor = descriptors[cull_data.descriptor_base + descriptor_index];
    if (descriptor.command_index >= cull_data.command_count)
        return;
    if (local_index == 0u) {
        shared_visible = descriptor_visible(descriptor) ? 1u : 0u;
        shared_write_base = 0u;
        if (shared_visible != 0u) {
            shared_write_base = cull_data.shadow_pass != 0u
                ? atomicAdd(shadow_commands[cull_data.shadow_command_base + descriptor.command_index].instance_count, descriptor.instance_count)
                : atomicAdd(main_commands[cull_data.main_command_base + descriptor.command_index].instance_count, descriptor.instance_count);
        }
    }
    barrier();
    if (shared_visible == 0u)
        return;

    uint destination_base = descriptor.first_visible + shared_write_base;
    for (uint i = local_index; i < descriptor.instance_count; i += 64u) {
        if (cull_data.shadow_pass != 0u) {
            shadow_visible[destination_base + i] = descriptor.first_instance + i;
            shadow_visible_draw_slots[destination_base + i] = descriptor.draw_slot;
            shadow_visible_layers[destination_base + i] = descriptor.visibility_layers;
        } else {
            main_visible[destination_base + i] = descriptor.first_instance + i;
            main_visible_draw_slots[destination_base + i] = descriptor.draw_slot;
            main_visible_layers[destination_base + i] = descriptor.visibility_layers;
        }
    }
}
