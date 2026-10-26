#[versions]
legacy = "";
grouped = "#define T3D_INSTANCE_CULL_PREFIX 1";
command_owner = "#define T3D_INSTANCE_CULL_PREFIX 1\n#define T3D_INSTANCE_CULL_COMMAND_OWNER 1";

#[compute]
#version 450
#VERSION_DEFINES
#ifndef T3D_INSTANCE_CULL_PREFIX
#define T3D_INSTANCE_CULL_PREFIX 0
#endif
#ifndef T3D_INSTANCE_CULL_COMMAND_OWNER
#define T3D_INSTANCE_CULL_COMMAND_OWNER 0
#endif
// Every instance in a workgroup uses the same view-projection planes.
// An owner lane prepares each active view once, and a workgroup barrier
// publishes the planes before any descriptor or instance visibility checks.
#ifndef T3D_INSTANCE_CULL_SHARED_FRUSTUM
#define T3D_INSTANCE_CULL_SHARED_FRUSTUM 1
#endif

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
layout(set = 0, binding = 11, std430) buffer MainVisibleFlagBuffer {
    uint main_visible_flags[];
};
layout(set = 0, binding = 12, std430) buffer ShadowVisibleFlagBuffer {
    uint shadow_visible_flags[];
};
#if T3D_INSTANCE_CULL_COMMAND_OWNER
// Per-pass CSR: [command_count+1 offsets][descriptor_count descriptor indices].
layout(set = 0, binding = 13, std430) readonly buffer CommandOwnerIndexBuffer {
    uint owner_words[];
};
#endif

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

#if T3D_INSTANCE_CULL_SHARED_FRUSTUM
shared vec4 shared_frustum_planes[12];
#endif
shared uint shared_visible;
shared uint shared_write_base;
shared uint shared_count;
shared uint shared_flags;
#if T3D_INSTANCE_CULL_PREFIX
shared uint shared_exclusive_total;
#endif
#if T3D_INSTANCE_CULL_PREFIX
// Eight independent 8-lane masks avoid both device subgroup dependencies and
// shared-memory atomic contention. The workgroup is fixed at 64 invocations.
shared uint shared_candidate_bits[64];
shared uint shared_octet_masks[8];
shared uint shared_octet_bases[8];
#endif

uint descriptor_draw_flags(InstanceCullDescriptor descriptor, vec3 fade_position) {
    uint flags = descriptor.flags;
    float margin = descriptor.bounds_center.w;
    bool final_preselected = (descriptor.flags & T3D_RD_INSTANCE_LOD_PRESELECTED) != 0u && descriptor.lod_end > 0.001;
    if (cull_data.shadow_pass == 0u && (margin > 0.0 || final_preselected)) {
        // For preselected cells, use each instance's world position. Fading an
        // entire cell based on its center creates large synchronized pop-outs.
        // Other descriptor categories retain their existing cell-level policy.
        vec3 camera_center = vec3(frame_data.camera_position[0].w, frame_data.camera_position[1].w, frame_data.motion_params.w);
        vec3 delta = fade_position - camera_center;
        float distance_sq = dot(delta, delta);
        float fade = 1.0;
        if (margin > 0.0) {
            float begin_end = descriptor.lod_begin + margin;
            float end_begin = max(0.0, descriptor.lod_end - margin);
            bool near_begin = descriptor.lod_begin > 0.001 && distance_sq < begin_end * begin_end;
            bool near_end = descriptor.lod_end > 0.001 && distance_sq > end_begin * end_begin;
            if (near_begin || near_end) {
                float distance = sqrt(distance_sq);
                if (near_begin) fade = min(fade, (distance - descriptor.lod_begin) / margin);
                if (near_end) fade = min(fade, (descriptor.lod_end - distance) / margin);
            }
        } else if (final_preselected && distance_sq >= descriptor.lod_end * descriptor.lod_end) {
            fade = 0.0;
        }
        uint fade_unorm = uint(floor(clamp(fade, 0.0, 1.0) * 65535.0 + 0.5));
        flags = (flags & ~0x00ffff00u) | (1u << 24u) | (fade_unorm << 8u);
    }
    return flags;
}

bool aabb_inside_plane(vec3 c, vec3 e, vec4 plane) {
    return dot(plane.xyz, c) + plane.w + dot(abs(plane.xyz), e) >= 0.0;
}

bool aabb_visible_view(vec3 c, vec3 e, uint view_index) {
    // Test the AABB support point against Vulkan clip planes rather than
    // transforming eight corners for every admitted instance.
#if T3D_INSTANCE_CULL_SHARED_FRUSTUM
    uint base = min(view_index, 1u) * 6u;
    return aabb_inside_plane(c, e, shared_frustum_planes[base])
        && aabb_inside_plane(c, e, shared_frustum_planes[base + 1u])
        && aabb_inside_plane(c, e, shared_frustum_planes[base + 2u])
        && aabb_inside_plane(c, e, shared_frustum_planes[base + 3u])
        && aabb_inside_plane(c, e, shared_frustum_planes[base + 4u])
        && aabb_inside_plane(c, e, shared_frustum_planes[base + 5u]);
#else
    mat4 rows = transpose(frame_data.view_projection[min(view_index, 1u)]);
    return aabb_inside_plane(c, e, rows[3] + rows[0])
        && aabb_inside_plane(c, e, rows[3] - rows[0])
        && aabb_inside_plane(c, e, rows[3] + rows[1])
        && aabb_inside_plane(c, e, rows[3] - rows[1])
        && aabb_inside_plane(c, e, rows[3] - rows[2])
        && aabb_inside_plane(c, e, rows[2]);
#endif
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

bool instance_visible(InstanceCullDescriptor descriptor, uint record_index, out vec3 instance_position) {
    InstanceRecord record = records[record_index];
    instance_position = record.position;
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
#if T3D_INSTANCE_CULL_SHARED_FRUSTUM
    // mode is dispatch-wide, so all 64 lanes reach this barrier. A workgroup
    // can exit uniformly on invalid command/descriptor indices only after it.
    if (local_index < clamp(cull_data.view_count, 1u, 2u)) {
        mat4 rows = transpose(frame_data.view_projection[local_index]);
        uint base = local_index * 6u;
        shared_frustum_planes[base] = rows[3] + rows[0];
        shared_frustum_planes[base + 1u] = rows[3] - rows[0];
        shared_frustum_planes[base + 2u] = rows[3] + rows[1];
        shared_frustum_planes[base + 3u] = rows[3] - rows[1];
        shared_frustum_planes[base + 4u] = rows[3] - rows[2];
        shared_frustum_planes[base + 5u] = rows[2];
    }
    barrier();
#endif
#if T3D_INSTANCE_CULL_COMMAND_OWNER
    uint command_index = gl_WorkGroupID.x;
    if (command_index >= cull_data.command_count)
        return;
    if (local_index == 0u) shared_exclusive_total = 0u;
    barrier();
    uint offset_base = cull_data.reserved_1;
    uint index_base = offset_base + cull_data.command_count + 1u;
    uint first = owner_words[offset_base + command_index];
    uint last = owner_words[offset_base + command_index + 1u];
    for (uint cursor = first; cursor < last; ++cursor) {
        uint descriptor_index = owner_words[index_base + cursor];
        // CPU validates the table before publication. Every lane uses the same index.
        InstanceCullDescriptor descriptor = descriptors[cull_data.descriptor_base + descriptor_index];
#else
    uint descriptor_index = gl_WorkGroupID.x;
    if (descriptor_index >= cull_data.descriptor_count)
        return;
    InstanceCullDescriptor descriptor = descriptors[cull_data.descriptor_base + descriptor_index];
    if (descriptor.command_index >= cull_data.command_count)
        return;
#endif
    if (local_index == 0u) {
        shared_visible = descriptor_visible(descriptor) ? 1u : 0u;
        shared_flags = shared_visible != 0u ? descriptor_draw_flags(descriptor, descriptor.bounds_center.xyz) : 0u;
#if T3D_INSTANCE_CULL_PREFIX && !T3D_INSTANCE_CULL_COMMAND_OWNER
        shared_exclusive_total = 0u;
#endif
    }
    barrier();
    if (shared_visible == 0u) {
#if T3D_INSTANCE_CULL_COMMAND_OWNER
        barrier();
        continue;
#else
        return;
#endif
    }

    // Compact each 64-instance chunk in shared memory. Reserve globally once per
    // chunk rather than once per surviving instance, including for grouped draws.
    for (uint chunk = 0u; chunk < descriptor.instance_count; chunk += 64u) {
#if !T3D_INSTANCE_CULL_PREFIX
        if (local_index == 0u) shared_count = 0u;
        barrier();
#endif
        uint i = chunk + local_index;
        bool visible = false;
        vec3 instance_position = vec3(0.0);
        if (i < descriptor.instance_count) visible = instance_visible(descriptor, descriptor.first_instance + i, instance_position);
        uint local_write = 0u;
#if T3D_INSTANCE_CULL_PREFIX
        // All 64 lanes participate, including inactive lanes in a short tail.
        shared_candidate_bits[local_index] = visible ? 1u : 0u;
        barrier();
        if (local_index < 8u) {
            uint mask = 0u;
            for (uint j = 0u; j < 8u; ++j)
                mask |= shared_candidate_bits[local_index * 8u + j] << j;
            shared_octet_masks[local_index] = mask;
        }
        barrier();
        if (local_index == 0u) {
            uint running = 0u;
            for (uint octet = 0u; octet < 8u; ++octet) {
                shared_octet_bases[octet] = running;
                running += uint(bitCount(shared_octet_masks[octet]));
            }
            shared_count = running;
        }
        barrier();
        uint octet = local_index >> 3u;
        uint lower_mask = (1u << (local_index & 7u)) - 1u;
        local_write = shared_octet_bases[octet] + uint(bitCount(shared_octet_masks[octet] & lower_mask));
#else
        if (visible) local_write = atomicAdd(shared_count, 1u);
        barrier();
#endif
        if (local_index == 0u) {
#if T3D_INSTANCE_CULL_COMMAND_OWNER
            shared_write_base = shared_exclusive_total;
            shared_exclusive_total += shared_count;
#else
#if T3D_INSTANCE_CULL_PREFIX
            // Exclusive CPU topology provides a single writer in the grouped variant.
            if (cull_data.reserved_0 != 0u) {
                shared_write_base = shared_exclusive_total;
                shared_exclusive_total += shared_count;
            } else
#endif
            {
                shared_write_base = 0u;
                if (shared_count > 0u) {
                    shared_write_base = cull_data.shadow_pass != 0u
                        ? atomicAdd(shadow_commands[cull_data.shadow_command_base + descriptor.command_index].instance_count, shared_count)
                        : atomicAdd(main_commands[cull_data.main_command_base + descriptor.command_index].instance_count, shared_count);
                }
            }
#endif
        }
        barrier();
        if (visible) {
            uint destination = descriptor.first_visible + shared_write_base + local_write;
            bool preselected_fade = cull_data.shadow_pass == 0u
                && (descriptor.flags & T3D_RD_INSTANCE_LOD_PRESELECTED) != 0u
                && (descriptor.bounds_center.w > 0.0 || descriptor.lod_end > 0.001);
            uint instance_flags = preselected_fade ? descriptor_draw_flags(descriptor, instance_position) : shared_flags;
            if (cull_data.shadow_pass != 0u) {
                shadow_visible[destination] = descriptor.first_instance + i;
                shadow_visible_draw_slots[destination] = descriptor.draw_slot;
                shadow_visible_layers[destination] = descriptor.visibility_layers;
                shadow_visible_flags[destination] = instance_flags;
            } else {
                main_visible[destination] = descriptor.first_instance + i;
                main_visible_draw_slots[destination] = descriptor.draw_slot;
                main_visible_layers[destination] = descriptor.visibility_layers;
                main_visible_flags[destination] = instance_flags;
            }
        }
        barrier();
    }
#if T3D_INSTANCE_CULL_PREFIX && !T3D_INSTANCE_CULL_COMMAND_OWNER
    if (local_index == 0u && cull_data.reserved_0 != 0u) {
        if (cull_data.shadow_pass != 0u)
            shadow_commands[cull_data.shadow_command_base + descriptor.command_index].instance_count = shared_exclusive_total;
        else
            main_commands[cull_data.main_command_base + descriptor.command_index].instance_count = shared_exclusive_total;
    }
#endif
#if T3D_INSTANCE_CULL_COMMAND_OWNER
        barrier();
    }
    if (local_index == 0u) {
        if (cull_data.shadow_pass != 0u)
            shadow_commands[cull_data.shadow_command_base + command_index].instance_count = shared_exclusive_total;
        else
            main_commands[cull_data.main_command_base + command_index].instance_count = shared_exclusive_total;
    }
#endif
}
