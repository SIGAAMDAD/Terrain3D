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
    vec4 local_bounds_center;
    vec4 local_bounds_extent;
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
struct InstanceRecord {
    vec3 position;
    uint quaternion01;
    uint quaternion23;
    uint scale01;
    uint scale2_flags;
    uint color_rgba8;
};
layout(set = 0, binding = 10, std430) readonly buffer InstanceRecords {
    InstanceRecord records[];
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
shared uint shared_count;

bool aabb_inside_plane(vec3 c, vec3 e, vec4 plane) {
    return dot(plane.xyz, c) + plane.w + dot(abs(plane.xyz), e) >= 0.0;
}

bool aabb_visible_view(vec3 c, vec3 e, uint view_index) {
    // Test the AABB support point against Vulkan clip planes rather than
    // transforming eight corners for every admitted instance.
    mat4 rows = transpose(frame_data.view_projection[min(view_index, 1u)]);
    return aabb_inside_plane(c, e, rows[3] + rows[0])
        && aabb_inside_plane(c, e, rows[3] - rows[0])
        && aabb_inside_plane(c, e, rows[3] + rows[1])
        && aabb_inside_plane(c, e, rows[3] - rows[1])
        && aabb_inside_plane(c, e, rows[3] - rows[2])
        && aabb_inside_plane(c, e, rows[2]);
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

vec3 rotate_quaternion(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

bool instance_visible(InstanceCullDescriptor descriptor, uint record_index) {
    InstanceRecord record = records[record_index];
    vec4 q = vec4(unpackSnorm2x16(record.quaternion01), unpackSnorm2x16(record.quaternion23));
    q = dot(q, q) > 0.000001 ? normalize(q) : vec4(0.0, 0.0, 0.0, 1.0);
    vec3 scale = vec3(unpackHalf2x16(record.scale01), unpackHalf2x16(record.scale2_flags & 0xffffu).x);
    vec3 center = record.position + rotate_quaternion(q, descriptor.local_bounds_center.xyz * scale);
    vec3 extent = abs(rotate_quaternion(q, vec3(scale.x, 0.0, 0.0))) * descriptor.local_bounds_extent.x
        + abs(rotate_quaternion(q, vec3(0.0, scale.y, 0.0))) * descriptor.local_bounds_extent.y
        + abs(rotate_quaternion(q, vec3(0.0, 0.0, scale.z))) * descriptor.local_bounds_extent.z;
    // Keep quantized transforms conservatively inside the CPU/GPU boundary.
    extent += vec3(0.001) + extent * 0.0001;
    for (uint view = 0u; view < clamp(cull_data.view_count, 1u, 2u); ++view) {
        if (aabb_visible_view(center, extent, view)) return true;
    }
    return false;
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
    }
    barrier();
    if (shared_visible == 0u)
        return;

    // Compact each 64-instance chunk in shared memory. Reserve globally once per
    // chunk rather than once per surviving instance, including for grouped draws.
    for (uint chunk = 0u; chunk < descriptor.instance_count; chunk += 64u) {
        if (local_index == 0u) shared_count = 0u;
        barrier();
        uint i = chunk + local_index;
        bool visible = false;
        if (i < descriptor.instance_count) visible = instance_visible(descriptor, descriptor.first_instance + i);
        uint local_write = 0u;
        if (visible) local_write = atomicAdd(shared_count, 1u);
        barrier();
        if (local_index == 0u) {
            shared_write_base = 0u;
            if (shared_count > 0u) {
                shared_write_base = cull_data.shadow_pass != 0u
                    ? atomicAdd(shadow_commands[cull_data.shadow_command_base + descriptor.command_index].instance_count, shared_count)
                    : atomicAdd(main_commands[cull_data.main_command_base + descriptor.command_index].instance_count, shared_count);
            }
        }
        barrier();
        if (visible) {
            uint destination = descriptor.first_visible + shared_write_base + local_write;
            if (cull_data.shadow_pass != 0u) {
                shadow_visible[destination] = descriptor.first_instance + i;
                shadow_visible_draw_slots[destination] = descriptor.draw_slot;
                shadow_visible_layers[destination] = descriptor.visibility_layers;
            } else {
                main_visible[destination] = descriptor.first_instance + i;
                main_visible_draw_slots[destination] = descriptor.draw_slot;
                main_visible_layers[destination] = descriptor.visibility_layers;
            }
        }
        barrier();
    }
}
